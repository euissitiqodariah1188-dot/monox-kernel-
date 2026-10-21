/* MONOX Kernel - printk / panic / early UART
 *
 * Early UART: PL011 (ARM) compatible, base address from DTB.
 * After VFS is up, also writes to /dev/kmsg (if mounted).
 *
 * Bug-fix pass:
 *  - hang() was used without a declaration (compile error).
 *  - early_uart_init() was only ever called by head.S before the DTB had
 *    been parsed (UART address still 0) and never again => the kernel
 *    printed NOTHING. It is now called from monox_start() after the DTB
 *    scan, and it replays the log collected so far.
 *  - kvsnprintf did not support '-', widths containing '0' (10, 20...),
 *    precision (%.63s), h/hh/t/j length modifiers, '+', ' ', '#', '*'. An
 *    unsupported spec printed '?' and left its argument unconsumed, which
 *    shifted every following va_arg (garbage / bad %s pointers).
 *  - panic backtrace followed an arbitrary x29 (not a frame pointer when
 *    built with -fomit-frame-pointer) => nested fault inside the panic path.
 *  - UART busy-wait now has a timeout; baud is not reprogrammed when the
 *    firmware already enabled the UART.
 *  - printk is IRQ-safe and cannot deadlock against a fault taken while the
 *    log lock was held.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>

extern void hang(void) __noreturn;     /* arch/arm64/boot/head.S */

/* -------------------------------------------------------
 * Early PL011 UART (base set from DTB at boot)
 * -------------------------------------------------------*/
#define PL011_DR     0x000
#define PL011_FR     0x018
#define PL011_IBRD   0x024
#define PL011_FBRD   0x028
#define PL011_LCR_H  0x02C
#define PL011_CR     0x030
#define PL011_IMSC   0x038

#define PL011_FR_TXFF (1 << 5)  /* TX FIFO full */
#define PL011_FR_RXFE (1 << 4)  /* RX FIFO empty */
#define PL011_CR_EN   (1 << 0)  /* UART enable */
#define PL011_CR_TXE  (1 << 8)  /* TX enable */
#define PL011_CR_RXE  (1 << 9)  /* RX enable */

static volatile u8 *g_uart_base = NULL;

static __always_inline void uart_write8(u32 off, u8 val)
{
    if (!g_uart_base) return;
    WRITE_ONCE(*(volatile u8 *)(g_uart_base + off), val);
}

static __always_inline u32 uart_read32(u32 off)
{
    if (!g_uart_base) return 0;
    return READ_ONCE(*(volatile u32 *)(g_uart_base + off));
}

static __always_inline void uart_write32(u32 off, u32 val)
{
    if (!g_uart_base) return;
    WRITE_ONCE(*(volatile u32 *)(g_uart_base + off), val);
}

/* -------------------------------------------------------
 * early_uart_init - set UART base from DTB
 * (called before MMU by head.S; uses physical address)
 * -------------------------------------------------------*/
static bool g_uart_replayed = false;
static void klog_replay_to_uart(void);

void early_uart_init(void)
{
    /* UART base is set by the DTB scan in monox_start() */
    extern phys_addr_t g_uart_phys_base;
    extern void *phys_to_virt_early(phys_addr_t);

    if (!g_uart_phys_base) return;
    /* identity (physical) pointer before the MMU is on, and still valid
     * afterwards because the UART page is mapped at its identity alias */
    g_uart_base = (volatile u8 *)phys_to_virt_early(g_uart_phys_base);

    u32 cr = uart_read32(PL011_CR);
    if (!(cr & PL011_CR_EN)) {
        /* Firmware did not set the UART up: 115200 baud, assuming 48 MHz */
        uart_write32(PL011_CR,   0);    /* disable */
        uart_write32(PL011_IBRD, 26);   /* 48e6 / (16 * 115200) = 26.04 */
        uart_write32(PL011_FBRD, 3);    /* frac = 0.04 * 64 ~= 3 */
        uart_write32(PL011_LCR_H, (3 << 5) | (1 << 4)); /* 8N1, FIFO enable */
        uart_write32(PL011_IMSC, 0);
        uart_write32(PL011_CR,   PL011_CR_EN | PL011_CR_TXE | PL011_CR_RXE);
    } else {
        /* keep the bootloader's baud rate, just make sure TX is on */
        uart_write32(PL011_CR, cr | PL011_CR_TXE);
    }

    klog_replay_to_uart();
}

static void uart_putchar(char c)
{
    if (!g_uart_base) return;
    if (c == '\n') uart_putchar('\r');   /* CR first, then LF */
    /* Wait until TX FIFO not full - bounded, so a dead/absent UART cannot
     * hang the whole kernel */
    u32 spins = 2000000;
    while ((uart_read32(PL011_FR) & PL011_FR_TXFF) && spins--)
        cpu_relax();
    uart_write8(PL011_DR, (u8)c);
}

static void uart_puts(const char *s)
{
    while (*s) uart_putchar(*s++);
}

/* -------------------------------------------------------
 * Minimal vsnprintf for kernel use (no floating point)
 *
 * Supports: flags  - 0 + space #
 *           width  N or *
 *           prec   .N or .*
 *           length hh h l ll z t j
 *           conv   d i u x X o p c s %
 * -------------------------------------------------------*/
struct pf_out { char *buf; u32 sz; u32 pos; };

static void pf_putc(struct pf_out *o, char c)
{
    if (o->pos + 1 < o->sz) o->buf[o->pos++] = c;
}

static void pf_fill(struct pf_out *o, int n, char c)
{
    while (n-- > 0) pf_putc(o, c);
}

/* emit [prefix][zeros][digits] padded to 'width' */
static void pf_number(struct pf_out *o, const char *prefix, const char *digits,
                      int ndigits, int width, int prec, bool left, bool zero)
{
    int zeros = (prec > ndigits) ? prec - ndigits : 0;
    int plen  = (int)strlen(prefix);
    int total = plen + zeros + ndigits;
    int pad   = (width > total) ? width - total : 0;

    /* a precision disables the '0' flag (ISO C) */
    if (prec >= 0) zero = false;

    if (!left && !zero) pf_fill(o, pad, ' ');
    for (int i = 0; i < plen; i++) pf_putc(o, prefix[i]);
    if (!left && zero) pf_fill(o, pad, '0');
    pf_fill(o, zeros, '0');
    for (int i = 0; i < ndigits; i++) pf_putc(o, digits[i]);
    if (left) pf_fill(o, pad, ' ');
}

static int utoa_rev(u64 val, u32 base, bool upper, char *tmp)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (!val) tmp[n++] = '0';
    while (val) { tmp[n++] = digits[val % base]; val /= base; }
    return n;
}

static u32 kvsnprintf(char *buf, u32 sz, const char *fmt, __builtin_va_list ap)
{
    struct pf_out o = { buf, sz, 0 };

    if (!sz) return 0;

    while (*fmt) {
        if (*fmt != '%') { pf_putc(&o, *fmt++); continue; }
        fmt++;

        /* ---- flags ---- */
        bool left = false, zero = false, plus = false, space = false, alt = false;
        for (;; fmt++) {
            if      (*fmt == '-') left  = true;
            else if (*fmt == '0') zero  = true;
            else if (*fmt == '+') plus  = true;
            else if (*fmt == ' ') space = true;
            else if (*fmt == '#') alt   = true;
            else break;
        }

        /* ---- width ---- */
        int width = 0;
        if (*fmt == '*') {
            width = __builtin_va_arg(ap, int);
            if (width < 0) { left = true; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }

        /* ---- precision ---- */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = __builtin_va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
            }
        }

        /* ---- length ---- */
        int lenmod = 0;   /* 0=int 1=long/ll/z/t/j(64-bit) -1=short -2=char */
        for (;;) {
            if      (*fmt == 'l') { lenmod = 1; fmt++; }
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') { lenmod = 1; fmt++; }
            else if (*fmt == 'h') { lenmod = (lenmod == -1) ? -2 : -1; fmt++; }
            else break;
        }

        char tmp[72];
        char conv = *fmt;
        if (!conv) break;          /* dangling '%' at end of format */
        fmt++;

        switch (conv) {
        case 'd': case 'i': {
            s64 v;
            if      (lenmod == 1)  v = __builtin_va_arg(ap, s64);
            else if (lenmod == -1) v = (short)__builtin_va_arg(ap, int);
            else if (lenmod == -2) v = (signed char)__builtin_va_arg(ap, int);
            else                   v = __builtin_va_arg(ap, int);
            bool neg = v < 0;
            u64 uv = neg ? (0 - (u64)v) : (u64)v;
            int n = utoa_rev(uv, 10, false, tmp);
            char rev[72];
            for (int i = 0; i < n; i++) rev[i] = tmp[n - 1 - i];
            const char *prefix = neg ? "-" : plus ? "+" : space ? " " : "";
            if (prec == 0 && uv == 0) n = 0;
            pf_number(&o, prefix, rev, n, width, prec, left, zero);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            u64 v;
            if      (lenmod == 1)  v = __builtin_va_arg(ap, u64);
            else if (lenmod == -1) v = (unsigned short)__builtin_va_arg(ap, unsigned int);
            else if (lenmod == -2) v = (unsigned char)__builtin_va_arg(ap, unsigned int);
            else                   v = __builtin_va_arg(ap, unsigned int);
            u32 base = (conv == 'u') ? 10 : (conv == 'o') ? 8 : 16;
            int n = utoa_rev(v, base, conv == 'X', tmp);
            char rev[72];
            for (int i = 0; i < n; i++) rev[i] = tmp[n - 1 - i];
            const char *prefix = "";
            if (alt && v) prefix = (conv == 'x') ? "0x" : (conv == 'X') ? "0X" :
                                   (conv == 'o') ? "0" : "";
            if (prec == 0 && v == 0) n = 0;
            pf_number(&o, prefix, rev, n, width, prec, left, zero);
            break;
        }
        case 'p': {
            u64 v = (u64)__builtin_va_arg(ap, void *);
            int n = utoa_rev(v, 16, false, tmp);
            char rev[72];
            for (int i = 0; i < n; i++) rev[i] = tmp[n - 1 - i];
            /* pad to 16 hex digits */
            pf_number(&o, "0x", rev, n, width, 16, left, false);
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(ap, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!left) pf_fill(&o, pad, ' ');
            pf_putc(&o, c);
            if (left) pf_fill(&o, pad, ' ');
            break;
        }
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            while ((prec < 0 || slen < prec) && s[slen]) slen++;
            int pad = width > slen ? width - slen : 0;
            if (!left) pf_fill(&o, pad, ' ');
            for (int i = 0; i < slen; i++) pf_putc(&o, s[i]);
            if (left) pf_fill(&o, pad, ' ');
            break;
        }
        case '%':
            pf_putc(&o, '%');
            break;
        default:
            /* unknown conversion: print it literally, do NOT eat an arg */
            pf_putc(&o, '%');
            pf_putc(&o, conv);
            break;
        }
    }
    buf[o.pos] = '\0';
    return o.pos;
}

/* -------------------------------------------------------
 * Kernel log ring buffer
 * -------------------------------------------------------*/
#define KLOG_BUF_SZ  (1 << 17)  /* 128 KiB */
static char      g_klog_buf[KLOG_BUF_SZ];
static u32       g_klog_head = 0;
static u32       g_klog_len  = 0;
static spinlock_t g_klog_lock = SPIN_LOCK_INIT;

/* Caller must hold g_klog_lock (or be in the panic path) */
static void klog_write_locked(const char *s, u32 len)
{
    for (u32 i = 0; i < len; i++)
        g_klog_buf[(g_klog_head + i) % KLOG_BUF_SZ] = s[i];
    g_klog_head = (g_klog_head + len) % KLOG_BUF_SZ;
    g_klog_len  = MIN(g_klog_len + len, (u32)KLOG_BUF_SZ);
}

/* Take the log lock, but never deadlock: if the lock is held by code we
 * interrupted (e.g. a fault taken inside printk) give up after a while and
 * go on without it. Returns true if the lock was really taken. */
static bool klog_lock(u64 *flags)
{
    *flags = local_irq_save();
    for (u32 i = 0; i < 200000; i++) {
        if (spin_trylock(&g_klog_lock))
            return true;
        cpu_relax();
    }
    return false;
}

static void klog_unlock(bool locked, u64 flags)
{
    if (locked) spin_unlock(&g_klog_lock);
    local_irq_restore(flags);
}

/* Output everything logged before the UART existed */
static void klog_replay_to_uart(void)
{
    if (g_uart_replayed || !g_uart_base) return;
    g_uart_replayed = true;

    u32 len   = g_klog_len;
    u32 start = (g_klog_head + KLOG_BUF_SZ - len) % KLOG_BUF_SZ;
    for (u32 i = 0; i < len; i++)
        uart_putchar(g_klog_buf[(start + i) % KLOG_BUF_SZ]);
}

/* -------------------------------------------------------
 * monox_printk - main kernel log function
 * -------------------------------------------------------*/
/* A plain char matrix, NOT an array of string pointers: this runs before the
 * MMU is on, where a statically initialised pointer holds an unmapped HIGH
 * address (first printk => data abort => silent hang). */
static const char LEVEL_STR[8][6] = {
    "EMERG", "ALERT", "CRIT ", "ERROR",
    "WARN ", "NOTIC", "INFO ", "DEBUG"
};

void monox_printk(const char *level, const char *fmt, ...)
{
    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    u32 n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);

    u8 lv = (u8)(*level - '0');
    if (lv > 7) lv = 6;

    char header[32];
    /* Simple "[LEVEL] " prefix */
    u32 hlen = 0;
    const char *ls = LEVEL_STR[lv];   /* points into the matrix (PC-relative) */
    header[hlen++] = '[';
    for (u32 i = 0; ls[i] && hlen < 28; i++) header[hlen++] = ls[i];
    header[hlen++] = ']';
    header[hlen++] = ' ';
    header[hlen]   = '\0';

    u64 flags;
    bool locked = klog_lock(&flags);

    klog_write_locked(header, hlen);
    klog_write_locked(buf, n);
    if (g_uart_base) {
        g_uart_replayed = true;      /* log is current from here on */
        uart_puts(header);
        uart_puts(buf);
    }

    klog_unlock(locked, flags);
}

/* -------------------------------------------------------
 * monox_dump_backtrace - walk the frame-pointer chain
 * (needs -fno-omit-frame-pointer, set in the Makefile). Every frame is
 * validated, otherwise a garbage x29 would fault inside the panic path.
 * -------------------------------------------------------*/
void monox_dump_backtrace(void)
{
    u64 fp, sp;
    __asm__ volatile("mov %0, x29" : "=r"(fp));
    __asm__ volatile("mov %0, sp"  : "=r"(sp));

    pr_crit("Backtrace:\n");
    for (int depth = 0; depth < 16 && fp; depth++) {
        /* 16-byte aligned, above the current SP, within 1 MiB of it */
        if ((fp & 0xF) || fp < sp || fp - sp > (1UL << 20))
            break;
        u64 *frame = (u64 *)fp;
        u64 lr     = frame[1];
        u64 next   = frame[0];
        pr_crit("  #%-2d [0x%016llx]\n", depth, lr);
        if (next <= fp) break;
        fp = next;
    }
}

/* -------------------------------------------------------
 * monox_panic
 * -------------------------------------------------------*/
void __noreturn monox_panic(const char *fmt, ...)
{
    static volatile u32 panicking = 0;

    local_irq_disable();

    if (panicking) {                 /* panic inside panic: give up quietly */
        uart_puts("\n*** nested panic, halting ***\n");
        hang();
    }
    panicking = 1;

    char buf[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);

    uart_puts("\n\n*** MONOX KERNEL PANIC ***\n");
    uart_puts(buf);
    uart_puts("\n");

    monox_dump_backtrace();

    uart_puts("\nSystem halted.\n");
    hang();
    __builtin_unreachable();
}
