#ifndef ARCH_S390_H
#define ARCH_S390_H

#define FIO_ARCH	(arch_s390)

#ifndef __NR_ioprio_set
#define __NR_ioprio_set		282
#define __NR_ioprio_get		283
#endif

#ifndef __NR_fadvise64
#define __NR_fadvise64		253
#endif

#ifndef __NR_sys_splice
#define __NR_sys_splice		306
#define __NR_sys_tee		308
#define __NR_sys_vmsplice	309
#endif

#define nop		asm volatile ("diag 0,0,68" : : : "memory")
#define read_barrier()	asm volatile("bcr 15,0" : : : "memory")
#define write_barrier()	asm volatile("bcr 15,0" : : : "memory")

#endif
