#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <libgen.h>
#include <math.h>

#include "fio.h"
#include "smalloc.h"
#include "diskutil.h"

static int last_majdev, last_mindev;
static struct disk_util *last_du;

FLIST_HEAD(disk_list);

static struct disk_util *__init_per_file_disk_util(struct thread_data *td,
		int majdev, int mindev, char *path);

static void disk_util_free(struct disk_util *du)
{
	if (du == last_du)
		last_du = NULL;

	while (!flist_empty(&du->slaves)) {
		struct disk_util *slave;

		slave = flist_entry(du->slaves.next, struct disk_util, slavelist);
		flist_del(&slave->slavelist);
		slave->users--;
	}

	fio_mutex_remove(du->lock);
	sfree(du);
}

static int get_io_ticks(struct disk_util *du, struct disk_util_stat *dus)
{
	unsigned in_flight;
	unsigned long long sectors[2];
	char line[256];
	FILE *f;
	char *p;
	int ret;

	dprint(FD_DISKUTIL, "open stat file: %s\n", du->path);

	f = fopen(du->path, "r");
	if (!f)
		return 1;

	p = fgets(line, sizeof(line), f);
	if (!p) {
		fclose(f);
		return 1;
	}

	dprint(FD_DISKUTIL, "%s: %s", du->path, p);

	ret = sscanf(p, "%u %u %llu %u %u %u %llu %u %u %u %u\n", &dus->ios[0],
					&dus->merges[0], &sectors[0],
					&dus->ticks[0], &dus->ios[1],
					&dus->merges[1], &sectors[1],
					&dus->ticks[1], &in_flight,
					&dus->io_ticks, &dus->time_in_queue);
	fclose(f);
	dprint(FD_DISKUTIL, "%s: stat read ok? %d\n", du->path, ret == 1);
	dus->sectors[0] = sectors[0];
	dus->sectors[1] = sectors[1];
	return ret != 11;
}

static void update_io_tick_disk(struct disk_util *du)
{
	struct disk_util_stat __dus, *dus, *ldus;
	struct timeval t;

	if (!du->users)
		return;
	if (get_io_ticks(du, &__dus))
		return;

	dus = &du->dus;
	ldus = &du->last_dus;

	dus->sectors[0] += (__dus.sectors[0] - ldus->sectors[0]);
	dus->sectors[1] += (__dus.sectors[1] - ldus->sectors[1]);
	dus->ios[0] += (__dus.ios[0] - ldus->ios[0]);
	dus->ios[1] += (__dus.ios[1] - ldus->ios[1]);
	dus->merges[0] += (__dus.merges[0] - ldus->merges[0]);
	dus->merges[1] += (__dus.merges[1] - ldus->merges[1]);
	dus->ticks[0] += (__dus.ticks[0] - ldus->ticks[0]);
	dus->ticks[1] += (__dus.ticks[1] - ldus->ticks[1]);
	dus->io_ticks += (__dus.io_ticks - ldus->io_ticks);
	dus->time_in_queue += (__dus.time_in_queue - ldus->time_in_queue);

	fio_gettime(&t, NULL);
	dus->msec += mtime_since(&du->time, &t);
	memcpy(&du->time, &t, sizeof(t));
	memcpy(ldus, &__dus, sizeof(__dus));
}

void update_io_ticks(void)
{
	struct flist_head *entry;
	struct disk_util *du;

	dprint(FD_DISKUTIL, "update io ticks\n");

	flist_for_each(entry, &disk_list) {
		du = flist_entry(entry, struct disk_util, list);
		update_io_tick_disk(du);
	}
}

static struct disk_util *disk_util_exists(int major, int minor)
{
	struct flist_head *entry;
	struct disk_util *du;

	flist_for_each(entry, &disk_list) {
		du = flist_entry(entry, struct disk_util, list);

		if (major == du->major && minor == du->minor)
			return du;
	}

	return NULL;
}

static int get_device_numbers(char *file_name, int *maj, int *min)
{
	struct stat st;
	int majdev, mindev;
	char tempname[PATH_MAX], *p;

	if (!lstat(file_name, &st)) {
		if (S_ISBLK(st.st_mode)) {
			majdev = major(st.st_rdev);
			mindev = minor(st.st_rdev);
		} else if (S_ISCHR(st.st_mode)) {
			majdev = major(st.st_rdev);
			mindev = minor(st.st_rdev);
			if (fio_lookup_raw(st.st_rdev, &majdev, &mindev))
				return -1;
		} else if (S_ISFIFO(st.st_mode))
			return -1;
		else {
			majdev = major(st.st_dev);
			mindev = minor(st.st_dev);
		}
	} else {
		/*
		 * must be a file, open "." in that path
		 */
		strncpy(tempname, file_name, PATH_MAX - 1);
		p = dirname(tempname);
		if (stat(p, &st)) {
			perror("disk util stat");
			return -1;
		}

		majdev = major(st.st_dev);
		mindev = minor(st.st_dev);
	}

	*min = mindev;
	*maj = majdev;

	return 0;
}

static int read_block_dev_entry(char *path, int *maj, int *min)
{
	char line[256], *p;
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		perror("open path");
		return 1;
	}

	p = fgets(line, sizeof(line), f);
	fclose(f);

	if (!p)
		return 1;

	if (sscanf(p, "%u:%u", maj, min) != 2)
		return 1;

	return 0;
}

static void find_add_disk_slaves(struct thread_data *td, char *path,
				 struct disk_util *masterdu)
{
	DIR *dirhandle = NULL;
	struct dirent *dirent = NULL;
	char slavesdir[PATH_MAX], temppath[PATH_MAX], slavepath[PATH_MAX];
	struct disk_util *slavedu = NULL;
	int majdev, mindev;
	ssize_t linklen;

	sprintf(slavesdir, "%s/%s", path, "slaves");
	dirhandle = opendir(slavesdir);
	if (!dirhandle)
		return;

	while ((dirent = readdir(dirhandle)) != NULL) {
		if (!strcmp(dirent->d_name, ".") ||
		    !strcmp(dirent->d_name, ".."))
			continue;

		sprintf(temppath, "%s%s%s", slavesdir, FIO_OS_PATH_SEPARATOR, dirent->d_name);
		/* Can we always assume that the slaves device entries
		 * are links to the real directories for the slave
		 * devices?
		 */
		linklen = readlink(temppath, slavepath, PATH_MAX - 0);
		if (linklen  < 0) {
			perror("readlink() for slave device.");
			return;
		}
		slavepath[linklen] = '\0';

		sprintf(temppath, "%s/%s/dev", slavesdir, slavepath);
		if (read_block_dev_entry(temppath, &majdev, &mindev)) {
			perror("Error getting slave device numbers.");
			return;
		}

		/*
		 * See if this maj,min already exists
		 */
		slavedu = disk_util_exists(majdev, mindev);
		if (slavedu)
			continue;

		sprintf(temppath, "%s%s%s", slavesdir, FIO_OS_PATH_SEPARATOR, slavepath);
		__init_per_file_disk_util(td, majdev, mindev, temppath);
		slavedu = disk_util_exists(majdev, mindev);

		/* Should probably use an assert here. slavedu should
		 * always be present at this point. */
		if (slavedu) {
			slavedu->users++;
			flist_add_tail(&slavedu->slavelist, &masterdu->slaves);
		}
	}

	closedir(dirhandle);
}

static struct disk_util *disk_util_add(struct thread_data *td, int majdev,
				       int mindev, char *path)
{
	struct disk_util *du, *__du;
	struct flist_head *entry;

	dprint(FD_DISKUTIL, "add maj/min %d/%d: %s\n", majdev, mindev, path);

	du = smalloc(sizeof(*du));
	memset(du, 0, sizeof(*du));
	INIT_FLIST_HEAD(&du->list);
	sprintf(du->path, "%s/stat", path);
	strncpy((char *) du->dus.name, basename(path), FIO_DU_NAME_SZ);
	du->sysfs_root = path;
	du->major = majdev;
	du->minor = mindev;
	INIT_FLIST_HEAD(&du->slavelist);
	INIT_FLIST_HEAD(&du->slaves);
	du->lock = fio_mutex_init(1);
	du->users = 0;

	flist_for_each(entry, &disk_list) {
		__du = flist_entry(entry, struct disk_util, list);

		dprint(FD_DISKUTIL, "found %s in list\n", __du->dus.name);

		if (!strcmp((char *) du->dus.name, (char *) __du->dus.name)) {
			disk_util_free(du);
			return __du;
		}
	}

	dprint(FD_DISKUTIL, "add %s to list\n", du->dus.name);

	fio_gettime(&du->time, NULL);
	get_io_ticks(du, &du->last_dus);

	flist_add_tail(&du->list, &disk_list);
	find_add_disk_slaves(td, path, du);
	return du;
}

static int check_dev_match(int majdev, int mindev, char *path)
{
	int major, minor;

	if (read_block_dev_entry(path, &major, &minor))
		return 1;

	if (majdev == major && mindev == minor)
		return 0;

	return 1;
}

static int find_block_dir(int majdev, int mindev, char *path, int link_ok)
{
	struct dirent *dir;
	struct stat st;
	int found = 0;
	DIR *D;

	D = opendir(path);
	if (!D)
		return 0;

	while ((dir = readdir(D)) != NULL) {
		char full_path[256];

		if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, ".."))
			continue;

		sprintf(full_path, "%s%s%s", path, FIO_OS_PATH_SEPARATOR, dir->d_name);

		if (!strcmp(dir->d_name, "dev")) {
			if (!check_dev_match(majdev, mindev, full_path)) {
				found = 1;
				break;
			}
		}

		if (link_ok) {
			if (stat(full_path, &st) == -1) {
				perror("stat");
				break;
			}
		} else {
			if (lstat(full_path, &st) == -1) {
				perror("stat");
				break;
			}
		}

		if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
			continue;

		found = find_block_dir(majdev, mindev, full_path, 0);
		if (found) {
			strcpy(path, full_path);
			break;
		}
	}

	closedir(D);
	return found;
}

static struct disk_util *__init_per_file_disk_util(struct thread_data *td,
						   int majdev, int mindev,
						   char *path)
{
	struct stat st;
	char tmp[PATH_MAX];
	char *p;

	/*
	 * If there's a ../queue/ directory there, we are inside a partition.
	 * Check if that is the case and jump back. For loop/md/dm etc we
	 * are already in the right spot.
	 */
	sprintf(tmp, "%s/../queue", path);
	if (!stat(tmp, &st)) {
		p = dirname(path);
		sprintf(tmp, "%s/queue", p);
		if (stat(tmp, &st)) {
			log_err("unknown sysfs layout\n");
			return NULL;
		}
		strncpy(tmp, p, PATH_MAX - 1);
		sprintf(path, "%s", tmp);
	}

	if (td->o.ioscheduler && !td->sysfs_root)
		td->sysfs_root = strdup(path);

	return disk_util_add(td, majdev, mindev, path);
}

static struct disk_util *init_per_file_disk_util(struct thread_data *td,
						 char *filename)
{

	char foo[PATH_MAX];
	struct disk_util *du;
	int mindev, majdev;

	if (get_device_numbers(filename, &majdev, &mindev))
		return NULL;

	dprint(FD_DISKUTIL, "%s belongs to maj/min %d/%d\n", filename, majdev,
			mindev);

	du = disk_util_exists(majdev, mindev);
	if (du) {
		if (td->o.ioscheduler && !td->sysfs_root)
			td->sysfs_root = strdup(du->sysfs_root);

		return du;
	}

	/*
	 * for an fs without a device, we will repeatedly stat through
	 * sysfs which can take oodles of time for thousands of files. so
	 * cache the last lookup and compare with that before going through
	 * everything again.
	 */
	if (mindev == last_mindev && majdev == last_majdev)
		return last_du;

	last_mindev = mindev;
	last_majdev = majdev;

	sprintf(foo, "/sys/block");
	if (!find_block_dir(majdev, mindev, foo, 1))
		return NULL;

	return __init_per_file_disk_util(td, majdev, mindev, foo);
}

static struct disk_util *__init_disk_util(struct thread_data *td,
					  struct fio_file *f)
{
	return init_per_file_disk_util(td, f->file_name);
}

void init_disk_util(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	if (!td->o.do_disk_util ||
	    (td->io_ops->flags & (FIO_DISKLESSIO | FIO_NODISKUTIL)))
		return;

	for_each_file(td, f, i)
		f->du = __init_disk_util(td, f);
}

static void show_agg_stats(struct disk_util_agg *agg, int terse)
{
	if (!agg->slavecount)
		return;

	if (!terse) {
		log_info(", aggrios=%u/%u, aggrmerge=%u/%u, aggrticks=%u/%u,"
				" aggrin_queue=%u, aggrutil=%3.2f%%",
				agg->ios[0] / agg->slavecount,
				agg->ios[1] / agg->slavecount,
				agg->merges[0] / agg->slavecount,
				agg->merges[1] / agg->slavecount,
				agg->ticks[0] / agg->slavecount,
				agg->ticks[1] / agg->slavecount,
				agg->time_in_queue / agg->slavecount,
				agg->max_util.u.f);
	} else {
		log_info(";slaves;%u;%u;%u;%u;%u;%u;%u;%3.2f%%",
				agg->ios[0] / agg->slavecount,
				agg->ios[1] / agg->slavecount,
				agg->merges[0] / agg->slavecount,
				agg->merges[1] / agg->slavecount,
				agg->ticks[0] / agg->slavecount,
				agg->ticks[1] / agg->slavecount,
				agg->time_in_queue / agg->slavecount,
				agg->max_util.u.f);
	}
}

static void aggregate_slaves_stats(struct disk_util *masterdu)
{
	struct disk_util_agg *agg = &masterdu->agg;
	struct disk_util_stat *dus;
	struct flist_head *entry;
	struct disk_util *slavedu;
	double util;

	flist_for_each(entry, &masterdu->slaves) {
		slavedu = flist_entry(entry, struct disk_util, slavelist);
		dus = &slavedu->dus;
		agg->ios[0] += dus->ios[0];
		agg->ios[1] += dus->ios[1];
		agg->merges[0] += dus->merges[0];
		agg->merges[1] += dus->merges[1];
		agg->sectors[0] += dus->sectors[0];
		agg->sectors[1] += dus->sectors[1];
		agg->ticks[0] += dus->ticks[0];
		agg->ticks[1] += dus->ticks[1];
		agg->time_in_queue += dus->time_in_queue;
		agg->slavecount++;

		util = (double) (100 * dus->io_ticks / (double) slavedu->dus.msec);
		/* System utilization is the utilization of the
		 * component with the highest utilization.
		 */
		if (util > agg->max_util.u.f)
			agg->max_util.u.f = util;

	}

	if (agg->max_util.u.f > 100.0)
		agg->max_util.u.f = 100.0;
}

void free_disk_util(void)
{
	struct disk_util *du;

	while (!flist_empty(&disk_list)) {
		du = flist_entry(disk_list.next, struct disk_util, list);
		flist_del(&du->list);
		disk_util_free(du);
	}

	last_majdev = last_mindev = -1;
}

void print_disk_util(struct disk_util_stat *dus, struct disk_util_agg *agg,
		     int terse)
{
	double util = 0;

	if (dus->msec)
		util = (double) 100 * dus->io_ticks / (double) dus->msec;
	if (util > 100.0)
		util = 100.0;

	if (!terse) {
		if (agg->slavecount)
			log_info("  ");

		log_info("  %s: ios=%u/%u, merge=%u/%u, ticks=%u/%u, "
			 "in_queue=%u, util=%3.2f%%", dus->name,
					dus->ios[0], dus->ios[1],
					dus->merges[0], dus->merges[1],
					dus->ticks[0], dus->ticks[1],
					dus->time_in_queue, util);
	} else {
		log_info(";%s;%u;%u;%u;%u;%u;%u;%u;%3.2f%%",
					dus->name, dus->ios[0], dus->ios[1],
					dus->merges[0], dus->merges[1],
					dus->ticks[0], dus->ticks[1],
					dus->time_in_queue, util);
	}

	/*
	 * If the device has slaves, aggregate the stats for
	 * those slave devices also.
	 */
	show_agg_stats(agg, terse);

	if (!terse)
		log_info("\n");
}

void show_disk_util(int terse)
{
	struct flist_head *entry;
	struct disk_util *du;

	if (flist_empty(&disk_list))
		return;

	if (!terse)
		log_info("\nDisk stats (read/write):\n");

	flist_for_each(entry, &disk_list) {
		du = flist_entry(entry, struct disk_util, list);

		aggregate_slaves_stats(du);
		print_disk_util(&du->dus, &du->agg, terse);
	}
}
