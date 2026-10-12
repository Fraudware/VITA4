/* VITA4 compat: the FreeBSD <sys/cdefs.h> pieces the USB headers use. */
#ifndef VD4_COMPAT_SYS_CDEFS_H
#define VD4_COMPAT_SYS_CDEFS_H
#ifndef __packed
#define __packed __attribute__((__packed__))
#endif
#ifndef __aligned
#define __aligned(x) __attribute__((__aligned__(x)))
#endif
#ifndef __BEGIN_DECLS
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
#endif
#endif
