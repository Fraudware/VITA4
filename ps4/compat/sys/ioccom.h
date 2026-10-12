/* VITA4 compat: FreeBSD ioctl encoding (sys/sys/ioccom.h, BSD-3-Clause).
 * The PS4 kernel is FreeBSD-based, but the OpenOrbis libc headers are musl
 * and define Linux-style _IO* macros; ugen ioctls need the FreeBSD values. */
#ifndef VD4_COMPAT_IOCCOM_H
#define VD4_COMPAT_IOCCOM_H
#include <sys/ioctl.h>
#undef _IOC
#undef _IO
#undef _IOR
#undef _IOW
#undef _IOWR
#undef IOCPARM_MASK
#undef IOC_VOID
#undef IOC_OUT
#undef IOC_IN
#undef IOC_INOUT
#define IOCPARM_SHIFT 13
#define IOCPARM_MASK ((1 << IOCPARM_SHIFT) - 1)
#define IOC_VOID 0x20000000UL
#define IOC_OUT 0x40000000UL
#define IOC_IN 0x80000000UL
#define IOC_INOUT (IOC_IN | IOC_OUT)
#define _IOC(inout, group, num, len) \
    ((unsigned long)((inout) | (((len) & IOCPARM_MASK) << 16) | ((group) << 8) | (num)))
#define _IO(g, n) _IOC(IOC_VOID, (g), (n), 0)
#define _IOWINT(g, n) _IOC(IOC_VOID, (g), (n), sizeof(int))
#define _IOR(g, n, t) _IOC(IOC_OUT, (g), (n), sizeof(t))
#define _IOW(g, n, t) _IOC(IOC_IN, (g), (n), sizeof(t))
#define _IOWR(g, n, t) _IOC(IOC_INOUT, (g), (n), sizeof(t))
#endif
