#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
typedef uint64_t u64; typedef int64_t s64; typedef uint32_t u32;
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


static int fails=0;
#define T(...) do{ char a[256],b[256]; snprintf(a,256,__VA_ARGS__); call(b,256,__VA_ARGS__); if(strcmp(a,b)){printf("FAIL: %s\n  libc=[%s]\n  mine=[%s]\n",#__VA_ARGS__,a,b);fails++;} }while(0)
static void call(char*buf,u32 sz,const char*fmt,...){va_list ap;va_start(ap,fmt);kvsnprintf(buf,sz,fmt,ap);va_end(ap);}
int main(){
 T("ZONE_%-6s base=0x%012llx span=%6llu", "DMA", 0x40000000ULL, 1024ULL);
 T("#%-2d [0x%016llx]", 3, 0xdeadbeefcafeULL);
 T("%.63s|", "label");  T("%.3s|", "abcdef"); T("%10u|%-10u|%010u", 42u,42u,42u);
 T("%hhu %hu %hhx", 300u, 70000u, 0x1ffu); T("%d %i %ld %lld", -5, 7, -123456789012L, -9223372036854775807LL);
 T("%zu %zx", (size_t)99, (size_t)255); T("%+d % d %+d", 5, 5, -5); T("%#x %#X %#o", 255u,255u,8u);
 T("%5s|%-5s|", "ab","cd"); T("%c%c", 'x','y'); T("100%%"); T("%*d|%-*d|", 5, 42, 5, 42); T("%.*s", 2, "hello");
 T("%08.3d|%5.3d", 7, 7); T("%u", 4294967295u); T("%llu", 18446744073709551615ULL); T("%x", 0u); T("%.0d|", 0);
 T("%20s|", "hi"); T("%100s|", "x"); T("%s", (char*)NULL ? "x" : "(null)");
 char a[64]; call(a,64,"%p", (void*)0x1234); printf("%%p -> %s\n", a);
 call(a,8,"truncate this string %d", 12345); printf("trunc -> [%s]\n", a);
 call(a,64,"%q%d", 5); printf("unknown conv -> [%s]\n", a);
 printf(fails?"FAILS=%d\n":"ALL OK\n", fails); return fails; }
