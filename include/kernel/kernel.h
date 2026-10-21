/* MONOX Kernel - Core Kernel Declarations
 * Target: ARM64 (ARMv8-A to ARMv9-A)
 *
 * NOTE (bug-fix pass): spinlocks, atomics and rwlocks were rewritten.
 *   - Only baseline ARMv8.0 LL/SC instructions are used (no LSE: the build
 *     uses -march=armv8-a, where STADD/LDADD do not assemble).
 *   - Ticket-lock layout now matches the assembly (owner:16 | next:16).
 *   - Locks are no-ops until the MMU/caches are on (exclusive accesses on
 *     Device memory are unsupported on many cores and hang forever).
 */

#ifndef _MONOX_KERNEL_H
#define _MONOX_KERNEL_H

#include <kernel/types.h>

/* -------------------------------------------------------
 * Doubly-linked list (embedded style, zero allocation)
 * -------------------------------------------------------*/
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name)  struct list_head name = LIST_HEAD_INIT(name)

static __always_inline void list_init(struct list_head *h)
{
    h->next = h;
    h->prev = h;
}

static __always_inline void __list_add(struct list_head *n,
                                       struct list_head *prev,
                                       struct list_head *next)
{
    next->prev = n;
    n->next    = next;
    n->prev    = prev;
    prev->next = n;
}

static __always_inline void list_add(struct list_head *n, struct list_head *h)
{
    __list_add(n, h, h->next);
}

static __always_inline void list_add_tail(struct list_head *n, struct list_head *h)
{
    __list_add(n, h->prev, h);
}

static __always_inline void list_del(struct list_head *entry)
{
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = NULL;
    entry->prev = NULL;
}

/* Remove and re-initialise (safe to list_del_init() again / test with list_empty) */
static __always_inline void list_del_init(struct list_head *entry)
{
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = entry;
    entry->prev = entry;
}

static __always_inline bool list_empty(const struct list_head *h)
{
    return h->next == h;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

#define list_for_each(pos, head) \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define list_for_each_safe(pos, n, head) \
    for ((pos) = (head)->next, (n) = (pos)->next; \
         (pos) != (head); \
         (pos) = (n), (n) = (pos)->next)

#define list_for_each_entry(pos, head, member) \
    for ((pos) = list_entry((head)->next, typeof(*(pos)), member); \
         &(pos)->member != (head); \
         (pos) = list_entry((pos)->member.next, typeof(*(pos)), member))

/* -------------------------------------------------------
 * Kernel panic / printk declarations
 * -------------------------------------------------------*/
void monox_panic(const char *fmt, ...) __noreturn;
void monox_printk(const char *level, const char *fmt, ...);
void monox_dump_backtrace(void);
void early_uart_init(void);   /* kernel/printk.c: (re)initialise the console UART */

#define KERN_EMERG   "0"
#define KERN_ALERT   "1"
#define KERN_CRIT    "2"
#define KERN_ERR     "3"
#define KERN_WARNING "4"
#define KERN_NOTICE  "5"
#define KERN_INFO    "6"
#define KERN_DEBUG   "7"

#define pr_emerg(fmt, ...)   monox_printk(KERN_EMERG,   fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)   monox_printk(KERN_ALERT,   fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)    monox_printk(KERN_CRIT,    fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)     monox_printk(KERN_ERR,     fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)    monox_printk(KERN_WARNING, fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)  monox_printk(KERN_NOTICE,  fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    monox_printk(KERN_INFO,    fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)   monox_printk(KERN_DEBUG,   fmt, ##__VA_ARGS__)

#define BUG()        monox_panic("BUG at %s:%d in %s()\n", \
                         __FILE__, __LINE__, __func__)
#define BUG_ON(c)    do { if (unlikely(c)) BUG(); } while (0)
#define WARN_ON(c)   do { if (unlikely(c)) \
                         pr_warn("WARN at %s:%d\n", __FILE__, __LINE__); } while (0)
#define ASSERT(c)    BUG_ON(!(c))

/* -------------------------------------------------------
 * IRQ helpers (implemented in arch/arm64/cpu/irq.c)
 * -------------------------------------------------------*/
void local_irq_disable(void);
void local_irq_enable(void);
u64  local_irq_save(void);
void local_irq_restore(u64 flags);
bool irqs_disabled(void);

#define IRQ_SAVE(flags)    ((flags) = local_irq_save())
#define IRQ_RESTORE(flags) local_irq_restore(flags)

/* Set to 1 by mmu_enable() once the MMU + caches are on. Before that point
 * the kernel is strictly single-CPU and exclusive-access instructions must
 * not be used (memory is Device-nGnRnE with the MMU off). */
extern volatile u32 g_kernel_locks_enabled;

/* -------------------------------------------------------
 * Spinlock (ticket lock, fair, ARM64 native)
 *
 * Memory layout == one 32-bit word:  bits[15:0]=owner  bits[31:16]=next
 * -------------------------------------------------------*/
typedef struct {
    volatile u16 owner;
    volatile u16 next;
} __aligned(4) spinlock_t;

#define SPIN_LOCK_INIT { .owner = 0, .next = 0 }

static __always_inline void spin_lock_init(spinlock_t *lock)
{
    lock->owner = 0;
    lock->next  = 0;
    wmb();
}

static __always_inline void spin_lock(spinlock_t *lock)
{
    u32 lockval, newval, tmp;

    if (unlikely(!READ_ONCE(g_kernel_locks_enabled)))
        return;

    __asm__ volatile(
        "   prfm    pstl1strm, %3           \n"
        "1: ldaxr   %w0, %3                 \n"
        "   add     %w1, %w0, %4            \n"
        "   stxr    %w2, %w1, %3            \n"
        "   cbnz    %w2, 1b                 \n"
        /* Did we get the lock? (owner == our ticket) */
        "   eor     %w1, %w0, %w0, ror #16  \n"
        "   cbz     %w1, 3f                 \n"
        /* No: spin on the owner half-word until it equals our ticket */
        "   sevl                            \n"
        "2: wfe                             \n"
        "   ldaxrh  %w2, %5                 \n"
        "   eor     %w1, %w2, %w0, lsr #16  \n"
        "   cbnz    %w1, 2b                 \n"
        "3:                                 \n"
        : "=&r"(lockval), "=&r"(newval), "=&r"(tmp),
          "+Q"(*(u32 *)lock)
        : "I"(1 << 16), "Q"(lock->owner)
        : "memory"
    );
}

static __always_inline void spin_unlock(spinlock_t *lock)
{
    u16 next_owner;

    if (unlikely(!READ_ONCE(g_kernel_locks_enabled)))
        return;

    next_owner = (u16)(lock->owner + 1);
    __asm__ volatile(
        "   stlrh   %w1, %0                 \n"
        : "=Q"(lock->owner)
        : "r"(next_owner)
        : "memory"
    );
}

static __always_inline bool spin_trylock(spinlock_t *lock)
{
    u32 lockval, tmp;

    if (unlikely(!READ_ONCE(g_kernel_locks_enabled)))
        return true;

    __asm__ volatile(
        "   prfm    pstl1strm, %2           \n"
        "1: ldaxr   %w0, %2                 \n"
        "   eor     %w1, %w0, %w0, ror #16  \n"
        "   cbnz    %w1, 2f                 \n"
        "   add     %w0, %w0, %3            \n"
        "   stxr    %w1, %w0, %2            \n"
        "   cbnz    %w1, 1b                 \n"
        "2:                                 \n"
        : "=&r"(lockval), "=&r"(tmp), "+Q"(*(u32 *)lock)
        : "I"(1 << 16)
        : "memory"
    );
    return tmp == 0;
}

/* IRQ-safe variants: use these for any lock that an interrupt handler
 * may also take, otherwise a handler that interrupts the lock holder
 * on the same CPU deadlocks forever. */
#define spin_lock_irqsave(lock, flags) \
    do { (flags) = local_irq_save(); spin_lock(lock); } while (0)
#define spin_unlock_irqrestore(lock, flags) \
    do { spin_unlock(lock); local_irq_restore(flags); } while (0)

/* -------------------------------------------------------
 * Atomic operations (LL/SC, ARMv8.0 baseline, full barrier)
 * -------------------------------------------------------*/
typedef struct { volatile s64 counter; } atomic64_t;
typedef struct { volatile s32 counter; } atomic_t;

#define ATOMIC_INIT(val)   { .counter = (val) }
#define ATOMIC64_INIT(val) { .counter = (val) }

static __always_inline s32 atomic_read(const atomic_t *v)
{
    return READ_ONCE(v->counter);
}

static __always_inline void atomic_set(atomic_t *v, s32 i)
{
    WRITE_ONCE(v->counter, i);
}

/* returns the NEW value */
static __always_inline s32 atomic_add_return(s32 i, atomic_t *v)
{
    s32 result;
    u32 tmp;
    __asm__ volatile(
        "1: ldxr    %w0, %2             \n"
        "   add     %w0, %w0, %w3       \n"
        "   stlxr   %w1, %w0, %2        \n"
        "   cbnz    %w1, 1b             \n"
        "   dmb     ish                 \n"
        : "=&r"(result), "=&r"(tmp), "+Q"(v->counter)
        : "Ir"(i)
        : "memory"
    );
    return result;
}

static __always_inline void atomic_add(s32 i, atomic_t *v)
{
    (void)atomic_add_return(i, v);
}

static __always_inline void atomic_sub(s32 i, atomic_t *v)
{
    (void)atomic_add_return(-i, v);
}

/* returns the OLD value (standard fetch_add semantics) */
static __always_inline s32 atomic_fetch_add(s32 i, atomic_t *v)
{
    return atomic_add_return(i, v) - i;
}

static __always_inline s32 atomic_inc_return(atomic_t *v)
{
    return atomic_add_return(1, v);
}

static __always_inline s32 atomic_dec_return(atomic_t *v)
{
    return atomic_add_return(-1, v);
}

static __always_inline bool atomic_dec_and_test(atomic_t *v)
{
    return atomic_dec_return(v) == 0;
}

/* compare-and-swap: returns the value observed in *v */
static __always_inline s32 atomic_cmpxchg(atomic_t *v, s32 old, s32 new_val)
{
    s32 oldval;
    u32 tmp;
    __asm__ volatile(
        "1: ldxr    %w1, %2             \n"
        "   cmp     %w1, %w3            \n"
        "   b.ne    2f                  \n"
        "   stlxr   %w0, %w4, %2        \n"
        "   cbnz    %w0, 1b             \n"
        "   dmb     ish                 \n"
        "2:                             \n"
        : "=&r"(tmp), "=&r"(oldval), "+Q"(v->counter)
        : "Ir"(old), "r"(new_val)
        : "cc", "memory"
    );
    return oldval;
}

static __always_inline s64 atomic64_read(const atomic64_t *v)
{
    return READ_ONCE(v->counter);
}

static __always_inline void atomic64_set(atomic64_t *v, s64 i)
{
    WRITE_ONCE(v->counter, i);
}

static __always_inline s64 atomic64_add_return(s64 i, atomic64_t *v)
{
    s64 result;
    u32 tmp;
    __asm__ volatile(
        "1: ldxr    %0, %2              \n"
        "   add     %0, %0, %3          \n"
        "   stlxr   %w1, %0, %2         \n"
        "   cbnz    %w1, 1b             \n"
        "   dmb     ish                 \n"
        : "=&r"(result), "=&r"(tmp), "+Q"(v->counter)
        : "Ir"(i)
        : "memory"
    );
    return result;
}

static __always_inline void atomic64_add(s64 i, atomic64_t *v)
{
    (void)atomic64_add_return(i, v);
}

static __always_inline s64 atomic64_fetch_add(s64 i, atomic64_t *v)
{
    return atomic64_add_return(i, v) - i;
}

/* -------------------------------------------------------
 * Read-write spinlock
 *   cntr > 0 : that many readers
 *   cntr = -1: one writer
 * (previous version never undid a failed fetch_add => lock state was
 *  corrupted by any contention)
 * -------------------------------------------------------*/
typedef struct {
    atomic_t cntr;
} rwlock_t;

#define RW_LOCK_INIT { .cntr = ATOMIC_INIT(0) }

static __always_inline void read_lock(rwlock_t *l)
{
    for (;;) {
        s32 v = atomic_read(&l->cntr);
        if (v >= 0 && atomic_cmpxchg(&l->cntr, v, v + 1) == v)
            return;
        __asm__ volatile("wfe" ::: "memory");
    }
}

static __always_inline void read_unlock(rwlock_t *l)
{
    atomic_sub(1, &l->cntr);
    dsb(ish);
    __asm__ volatile("sev" ::: "memory");
}

static __always_inline void write_lock(rwlock_t *l)
{
    for (;;) {
        if (atomic_read(&l->cntr) == 0 &&
            atomic_cmpxchg(&l->cntr, 0, -1) == 0)
            return;
        __asm__ volatile("wfe" ::: "memory");
    }
}

static __always_inline void write_unlock(rwlock_t *l)
{
    atomic_add(1, &l->cntr);
    dsb(ish);
    __asm__ volatile("sev" ::: "memory");
}

/* -------------------------------------------------------
 * CPU / cache operations
 * -------------------------------------------------------*/
static __always_inline void cpu_relax(void)
{
    __asm__ volatile("yield" ::: "memory");
}

static __always_inline u32 __dcache_line_size(void)
{
    u64 ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4U << ((ctr >> 16) & 0xF);          /* DminLine */
}

static __always_inline u32 __icache_line_size(void)
{
    u64 ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4U << (ctr & 0xF);                  /* IminLine */
}

static __always_inline void cache_flush_range(void *start, size_t len)
{
    uintptr_t line = __dcache_line_size();
    uintptr_t addr = (uintptr_t)start & ~(line - 1);
    uintptr_t end  = (uintptr_t)start + len;
    while (addr < end) {
        __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
        addr += line;
    }
    dsb(ish);
    isb();
}

static __always_inline void icache_invalidate_range(void *start, size_t len)
{
    uintptr_t line = __icache_line_size();
    uintptr_t addr = (uintptr_t)start & ~(line - 1);
    uintptr_t end  = (uintptr_t)start + len;
    while (addr < end) {
        __asm__ volatile("ic ivau, %0" :: "r"(addr) : "memory");
        addr += line;
    }
    dsb(ish);
    isb();
}

/* -------------------------------------------------------
 * Per-CPU helpers (SMP)
 * -------------------------------------------------------*/
#define MONOX_MAX_CPUS  16   /* must stay a power of two */

/* Aff0 of MPIDR, clamped so that it can never index outside the per-CPU
 * arrays (cpu_info, run queues...) on SoCs whose Aff0 exceeds 15. */
static __always_inline u32 smp_processor_id(void)
{
    u64 mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    return (u32)(mpidr & 0xFF) & (MONOX_MAX_CPUS - 1);
}

/* -------------------------------------------------------
 * Miscellaneous (freestanding libc subset)
 *
 * Out-of-line copies of memset/memcpy/memmove/memcmp live in
 * lib/string.c because GCC may emit calls to them on its own.
 * -------------------------------------------------------*/
static __always_inline void *memset(void *dst, int c, size_t n)
{
    u8 *d = dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

static __always_inline void *memcpy(void *dst, const void *src, size_t n)
{
    const u8 *s = src;
    u8 *d = dst;
    while (n--) *d++ = *s++;
    return dst;
}

static __always_inline void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d == s || !n) return dst;
    if (d < s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

static __always_inline int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *pa = a, *pb = b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

static __always_inline size_t strlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

static __always_inline int strcmp(const char *a, const char *b)
{
    while (*a && (*a == *b)) { a++; b++; }
    return (u8)*a - (u8)*b;
}

/* Fixed: the old version compared the (n+1)-th character when the first n
 * characters were equal, so strncmp("abcX","abcY",3) != 0. */
static __always_inline int strncmp(const char *a, const char *b, size_t n)
{
    while (n) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
        a++; b++; n--;
    }
    return 0;
}

static __always_inline char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

/* Fixed: now pads with NULs like ISO C (old version left the tail of
 * the destination uninitialised). */
static __always_inline char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

#endif /* _MONOX_KERNEL_H */
