#!/usr/bin/env python3
"""Generate x86-host-compilable copies of include/kernel/{types,kernel}.h.

The real headers contain AArch64 inline assembly (ticket locks, LL/SC atomics,
barriers). For host-side logic tests only those function bodies are swapped
for plain C / GCC builtins; every other line is copied verbatim.
usage: gen_host_headers.py <repo> <outdir>
"""
import sys, re, os
repo, out = sys.argv[1], sys.argv[2]
os.makedirs(out + "/kernel", exist_ok=True)

t = open(repo + "/include/kernel/types.h").read()
t = re.sub(r'(?m)^#define (mb|rmb|wmb)\(\).*', r'#define \1() __asm__ volatile("" ::: "memory")', t)
t = re.sub(r'(?m)^#define isb\(\).*',  '#define isb() __asm__ volatile("" ::: "memory")', t)
t = re.sub(r'(?m)^#define dsb\(opt\).*', '#define dsb(opt) __asm__ volatile("" ::: "memory")', t)
open(out + "/kernel/types.h", "w").write(t)

s = open(repo + "/include/kernel/kernel.h").read()
a = s.index("static __always_inline void spin_lock(spinlock_t *lock)")
b = s.index("/* IRQ-safe variants")
s = s[:a] + """static __always_inline void spin_lock(spinlock_t *l){ (void)l; }
static __always_inline void spin_unlock(spinlock_t *l){ (void)l; }
static __always_inline bool spin_trylock(spinlock_t *l){ (void)l; return true; }

""" + s[b:]
a = s.index("typedef struct { volatile s64 counter; } atomic64_t;")
b = s.index("/* -------------------------------------------------------\n * CPU / cache operations")
s = s[:a] + """typedef struct { volatile s64 counter; } atomic64_t;
typedef struct { volatile s32 counter; } atomic_t;
#define ATOMIC_INIT(val)   { .counter = (val) }
#define ATOMIC64_INIT(val) { .counter = (val) }
static __always_inline s32 atomic_read(const atomic_t *v){ return v->counter; }
static __always_inline void atomic_set(atomic_t *v, s32 i){ v->counter = i; }
static __always_inline s32 atomic_add_return(s32 i, atomic_t *v){ return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static __always_inline void atomic_add(s32 i, atomic_t *v){ (void)atomic_add_return(i, v); }
static __always_inline void atomic_sub(s32 i, atomic_t *v){ (void)atomic_add_return(-i, v); }
static __always_inline s32 atomic_fetch_add(s32 i, atomic_t *v){ return atomic_add_return(i, v) - i; }
static __always_inline s32 atomic_inc_return(atomic_t *v){ return atomic_add_return(1, v); }
static __always_inline s32 atomic_dec_return(atomic_t *v){ return atomic_add_return(-1, v); }
static __always_inline bool atomic_dec_and_test(atomic_t *v){ return atomic_dec_return(v) == 0; }
static __always_inline s32 atomic_cmpxchg(atomic_t *v, s32 o, s32 n){ return __sync_val_compare_and_swap(&v->counter, o, n); }
static __always_inline s64 atomic64_read(const atomic64_t *v){ return v->counter; }
static __always_inline void atomic64_set(atomic64_t *v, s64 i){ v->counter = i; }
static __always_inline s64 atomic64_add_return(s64 i, atomic64_t *v){ return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST); }
static __always_inline void atomic64_add(s64 i, atomic64_t *v){ (void)atomic64_add_return(i, v); }
static __always_inline s64 atomic64_fetch_add(s64 i, atomic64_t *v){ return atomic64_add_return(i, v) - i; }
typedef struct { atomic_t cntr; } rwlock_t;
#define RW_LOCK_INIT { .cntr = ATOMIC_INIT(0) }
static __always_inline void read_lock(rwlock_t *l){ atomic_add(1, &l->cntr); }
static __always_inline void read_unlock(rwlock_t *l){ atomic_sub(1, &l->cntr); }
static __always_inline void write_lock(rwlock_t *l){ atomic_set(&l->cntr, -1); }
static __always_inline void write_unlock(rwlock_t *l){ atomic_set(&l->cntr, 0); }

""" + s[b:]
a = s.index("static __always_inline void cpu_relax(void)")
b = s.index("/* -------------------------------------------------------\n * Miscellaneous")
s = s[:a] + """static __always_inline void cpu_relax(void){}
#define MONOX_MAX_CPUS 16
static __always_inline u32 smp_processor_id(void){ return 0; }
static __always_inline void cache_flush_range(void *s, size_t l){ (void)s; (void)l; }

""" + s[b:]
open(out + "/kernel/kernel.h", "w").write(s)
