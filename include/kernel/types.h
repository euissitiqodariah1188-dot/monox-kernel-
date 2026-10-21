/* MONOX Kernel - Core Type Definitions
 * Target: ARM64 (ARMv8-A to ARMv9-A)
 * License: MIT
 */

#ifndef _MONOX_TYPES_H
#define _MONOX_TYPES_H

/* -------------------------------------------------------
 * Standard integer types for ARM64 LP64 ABI
 * -------------------------------------------------------*/
typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef signed char         s8;
typedef signed short        s16;
typedef signed int          s32;
typedef signed long long    s64;

typedef u64  uintptr_t;
typedef s64  intptr_t;
typedef u64  size_t;
typedef s64  ssize_t;
typedef u64  phys_addr_t;
typedef u64  virt_addr_t;
typedef u64  dma_addr_t;
typedef s64  off_t;
typedef u32  pid_t;
typedef u32  uid_t;
typedef u32  gid_t;
typedef u32  mode_t;
typedef u32  dev_t;
typedef u64  ino_t;
typedef u64  nlink_t;
typedef u64  blksize_t;
typedef u64  blkcnt_t;
typedef s64  time_t;
typedef s64  clockid_t;
typedef u32  gfp_t;        /* memory allocation flags */
typedef u32  irq_t;
typedef u32  cpu_t;

/* Boolean */
typedef u8   bool;
#define true   ((bool)1)
#define false  ((bool)0)

/* NULL */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* -------------------------------------------------------
 * Compile-time helpers
 * -------------------------------------------------------*/
#define ARRAY_SIZE(arr)     (sizeof(arr) / sizeof((arr)[0]))
#define ALIGN(x, a)         (((x) + ((a) - 1)) & ~((a) - 1))
#define IS_ALIGNED(x, a)    (((x) & ((a) - 1)) == 0)
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))
#define CLAMP(val, lo, hi)  (MAX((lo), MIN((val), (hi))))
#define ROUND_UP(x, y)      (((x) + (y) - 1) / (y) * (y))
#define ROUND_DOWN(x, y)    ((x) / (y) * (y))
#define DIV_ROUND_UP(n, d)  (((n) + (d) - 1) / (d))
#define BIT(n)              (1ULL << (n))
#define GENMASK(h, l)       (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))

#define offsetof(type, member) __builtin_offsetof(type, member)
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

/* -------------------------------------------------------
 * Compiler attributes
 * -------------------------------------------------------*/
#define __packed            __attribute__((packed))
#define __aligned(x)        __attribute__((aligned(x)))
#define __noreturn          __attribute__((noreturn))
#define __noinline          __attribute__((noinline))
#define __always_inline     __attribute__((always_inline)) inline
#define __weak              __attribute__((weak))
#define __used              __attribute__((used))
#define __unused            __attribute__((unused))
#define __section(s)        __attribute__((section(s)))
#define __cold              __attribute__((cold))
#define __hot               __attribute__((hot))
#define __pure              __attribute__((pure))
#define __const             __attribute__((const))

#define likely(x)           __builtin_expect(!!(x), 1)
#define unlikely(x)         __builtin_expect(!!(x), 0)

/* -------------------------------------------------------
 * Memory barriers for ARM64
 * -------------------------------------------------------*/
#define mb()        __asm__ volatile("dmb ish"  ::: "memory")
#define rmb()       __asm__ volatile("dmb ishld" ::: "memory")
#define wmb()       __asm__ volatile("dmb ishst" ::: "memory")
#define barrier()   __asm__ volatile("" ::: "memory")
#define isb()       __asm__ volatile("isb" ::: "memory")
#define dsb(opt)    __asm__ volatile("dsb " #opt ::: "memory")

/* -------------------------------------------------------
 * Atomic read / write (relaxed, no barrier)
 * -------------------------------------------------------*/
#define READ_ONCE(x)       (*(volatile typeof(x) *)&(x))
#define WRITE_ONCE(x, val) (*(volatile typeof(x) *)&(x) = (val))

/* -------------------------------------------------------
 * Return-code convention
 * -------------------------------------------------------*/
#define MX_OK          0
#define MX_ENOMEM     -1   /* out of memory               */
#define MX_EINVAL     -2   /* invalid argument             */
#define MX_EBUSY      -3   /* resource busy                */
#define MX_ENODEV     -4   /* no such device               */
#define MX_EACCES     -5   /* permission denied            */
#define MX_EFAULT     -6   /* bad address                  */
#define MX_ENOENT     -7   /* no such file or directory    */
#define MX_EIO        -8   /* I/O error                    */
#define MX_ENOSYS     -9   /* function not implemented     */
#define MX_EEXIST    -10   /* file already exists          */
#define MX_ERANGE    -11   /* result out of range          */
#define MX_ETIMEDOUT -12   /* operation timed out          */
#define MX_EAGAIN    -13   /* try again                    */
#define MX_EOVERFLOW -14   /* value too large              */
#define MX_ENOSPC    -15   /* no space left on device      */
#define MX_EPERM     -16   /* operation not permitted      */
#define MX_ESRCH     -17   /* no such process              */
#define MX_EINTR     -18   /* interrupted system call      */
#define MX_EDEADLK   -19   /* resource deadlock would occur*/
#define MX_ETIME     -20   /* timer expired                */
#define MX_ENOTDIR   -21   /* not a directory              */

#define IS_ERR(x)    ((unsigned long)(x) > (unsigned long)(-4096))
#define ERR_PTR(e)   ((void *)(intptr_t)(e))
#define PTR_ERR(p)   ((intptr_t)(p))

/* -------------------------------------------------------
 * Kernel version
 * -------------------------------------------------------*/
#define MONOX_VERSION_MAJOR  1
#define MONOX_VERSION_MINOR  0
#define MONOX_VERSION_PATCH  0
#define MONOX_CODENAME       "Singleblaze"

#endif /* _MONOX_TYPES_H */
