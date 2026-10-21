/* MONOX Kernel - out-of-line freestanding mem*() routines
 *
 * GCC is allowed to emit calls to memcpy/memmove/memset/memcmp by itself
 * (struct copies, large "= {0}" initialisers, ...) even with -fno-builtin.
 * The versions in <kernel/kernel.h> are static inline and therefore have
 * no linkable symbol, which would give "undefined reference to `memset'"
 * at link time. These are the real symbols.
 *
 * They must NOT be turned back into calls to themselves by the optimiser:
 * the Makefile builds the tree with -fno-tree-loop-distribute-patterns and
 * the attribute below repeats that for safety.
 */

#include <kernel/types.h>

#define NO_LOOP_PATTERNS __attribute__((optimize("no-tree-loop-distribute-patterns")))

NO_LOOP_PATTERNS void *memset(void *dst, int c, size_t n)
{
    volatile u8 *d = (volatile u8 *)dst;
    while (n--) *d++ = (u8)c;
    return dst;
}

NO_LOOP_PATTERNS void *memcpy(void *dst, const void *src, size_t n)
{
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    while (n--) *d++ = *s++;
    return dst;
}

NO_LOOP_PATTERNS void *memmove(void *dst, const void *src, size_t n)
{
    volatile u8 *d = (volatile u8 *)dst;
    const volatile u8 *s = (const volatile u8 *)src;
    if (d == s || !n) return dst;
    if (d < s || d >= s + n) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

NO_LOOP_PATTERNS int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *pa = (const u8 *)a, *pb = (const u8 *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}
