/* MONOX Kernel - System Satpam (Kernel Integrity + Antivirus Guard)
 *
 * Responsibilities:
 *  1. Protect kernel .text section from runtime modification
 *  2. Validate system call arguments before dispatch
 *  3. Maintain a hash of critical kernel pages (runtime integrity)
 *  4. Detect and quarantine malicious processes
 *  5. Monitor memory regions for known virus/malware signatures
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "system_satpam.h"

/* -------------------------------------------------------
 * Kernel integrity: protected region table
 * -------------------------------------------------------*/
#define MAX_PROTECTED_REGIONS 64
#define HASH_BLOCK_SIZE       64   /* SHA-256 input block = 64 bytes */

struct protected_region {
    u64     va_start;
    u64     va_end;
    u8      expected_hash[32];   /* SHA-256 of the region */
    char    name[64];
    bool    valid;
    bool    alert_only;          /* if true, log but don't panic */
};

static struct protected_region g_regions[MAX_PROTECTED_REGIONS];
static u32                     g_nr_regions = 0;
static spinlock_t              g_region_lock = SPIN_LOCK_INIT;

/* -------------------------------------------------------
 * Malware signature database (data-driven, not hardcoded logic)
 * -------------------------------------------------------*/
#define MAX_SIGNATURES      512
#define MAX_SIG_LEN          32

struct malware_sig {
    u8      pattern[MAX_SIG_LEN];
    u8      mask[MAX_SIG_LEN];      /* 0xFF = must match, 0x00 = don't care */
    u32     len;
    char    name[64];
    bool    valid;
};

static struct malware_sig g_signatures[MAX_SIGNATURES];
static u32                g_nr_sigs = 0;
static spinlock_t         g_sig_lock = SPIN_LOCK_INIT;

/* -------------------------------------------------------
 * Quarantine list - banned processes
 * -------------------------------------------------------*/
#define MAX_QUARANTINE  128

struct quarantine_entry {
    pid_t   pid;
    char    name[64];
    u64     time;
    char    reason[128];
    bool    valid;
};

static struct quarantine_entry g_quarantine[MAX_QUARANTINE];
static u32                     g_nr_quarantined = 0;
static spinlock_t              g_qlock = SPIN_LOCK_INIT;

/* -------------------------------------------------------
 * Minimal SHA-256 implementation (no external dependency)
 * -------------------------------------------------------*/
static const u32 K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static __always_inline u32 rotr32(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

struct sha256_ctx {
    u32 state[8];
    u8  buf[64];
    u64 bitlen;
    u32 buflen;
};

static void sha256_init(struct sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

static void sha256_transform(u32 *s, const u8 *data)
{
    u32 a,b,c,d,e,f,g,h,t1,t2,m[64];
    for (u32 i = 0; i < 16; i++) {
        m[i] = ((u32)data[i*4]<<24)|((u32)data[i*4+1]<<16)|
               ((u32)data[i*4+2]<<8)|((u32)data[i*4+3]);
    }
    for (u32 i = 16; i < 64; i++) {
        u32 s0 = rotr32(m[i-15],7)^rotr32(m[i-15],18)^(m[i-15]>>3);
        u32 s1 = rotr32(m[i-2],17)^rotr32(m[i-2],19)^(m[i-2]>>10);
        m[i] = m[i-16]+s0+m[i-7]+s1;
    }
    a=s[0]; b=s[1]; c=s[2]; d=s[3];
    e=s[4]; f=s[5]; g=s[6]; h=s[7];
    for (u32 i = 0; i < 64; i++) {
        u32 S1  = rotr32(e,6)^rotr32(e,11)^rotr32(e,25);
        u32 ch  = (e&f)^((~e)&g);
        t1 = h+S1+ch+K256[i]+m[i];
        u32 S0  = rotr32(a,2)^rotr32(a,13)^rotr32(a,22);
        u32 maj = (a&b)^(a&c)^(b&c);
        t2 = S0+maj;
        h=g; g=f; f=e; e=d+t1;
        d=c; c=b; b=a; a=t1+t2;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d;
    s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
}

static void sha256_update(struct sha256_ctx *ctx, const u8 *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ctx->buf[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx->state, ctx->buf);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *ctx, u8 out[32])
{
    u32 i = ctx->buflen;
    ctx->buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->buf[i++] = 0;
        sha256_transform(ctx->state, ctx->buf);
        i = 0;
    }
    while (i < 56) ctx->buf[i++] = 0;
    ctx->bitlen += ctx->buflen * 8;
    u64 bl = ctx->bitlen;
    for (int j = 7; j >= 0; j--) { ctx->buf[56+j] = (u8)(bl & 0xFF); bl >>= 8; }
    sha256_transform(ctx->state, ctx->buf);
    for (int j = 0; j < 8; j++) {
        out[j*4  ] = (u8)(ctx->state[j] >> 24);
        out[j*4+1] = (u8)(ctx->state[j] >> 16);
        out[j*4+2] = (u8)(ctx->state[j] >>  8);
        out[j*4+3] = (u8)(ctx->state[j]      );
    }
}

static void compute_hash(const void *data, size_t len, u8 out[32])
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const u8 *)data, len);
    sha256_final(&ctx, out);
}

/* -------------------------------------------------------
 * sysatpam_init
 * -------------------------------------------------------*/
void sysatpam_init(void)
{
    spin_lock_init(&g_region_lock);
    spin_lock_init(&g_sig_lock);
    spin_lock_init(&g_qlock);
    memset(g_regions,    0, sizeof(g_regions));
    memset(g_signatures, 0, sizeof(g_signatures));
    memset(g_quarantine, 0, sizeof(g_quarantine));

    pr_info("SYSATPAM: Kernel integrity + antivirus guard online\n");
}

/* -------------------------------------------------------
 * sysatpam_protect_region - hash and protect a memory region
 * -------------------------------------------------------*/
int sysatpam_protect_region(u64 va_start, u64 va_end,
                              const char *name, bool alert_only)
{
    if (va_start >= va_end || !name) return MX_EINVAL;

    spin_lock(&g_region_lock);
    if (g_nr_regions >= MAX_PROTECTED_REGIONS) {
        spin_unlock(&g_region_lock);
        return MX_ENOSPC;
    }

    struct protected_region *r = NULL;
    for (u32 i = 0; i < MAX_PROTECTED_REGIONS; i++) {
        if (!g_regions[i].valid) { r = &g_regions[i]; break; }
    }
    if (!r) { spin_unlock(&g_region_lock); return MX_ENOSPC; }

    r->va_start   = va_start;
    r->va_end     = va_end;
    r->alert_only = alert_only;
    r->valid      = true;
    strncpy(r->name, name, 63);

    compute_hash((const void *)va_start, (size_t)(va_end - va_start),
                 r->expected_hash);
    g_nr_regions++;
    spin_unlock(&g_region_lock);

    pr_info("SYSATPAM: protecting region '%s' [0x%llx..0x%llx]\n",
            name, va_start, va_end);
    return MX_OK;
}

/* -------------------------------------------------------
 * sysatpam_verify_integrity - check all protected regions
 * -------------------------------------------------------*/
int sysatpam_verify_integrity(void)
{
    int violations = 0;
    u8  current_hash[32];

    spin_lock(&g_region_lock);
    for (u32 i = 0; i < MAX_PROTECTED_REGIONS; i++) {
        if (!g_regions[i].valid) continue;

        struct protected_region *r = &g_regions[i];
        compute_hash((const void *)r->va_start,
                     (size_t)(r->va_end - r->va_start),
                     current_hash);

        if (memcmp(current_hash, r->expected_hash, 32) != 0) {
            violations++;
            pr_crit("SYSATPAM: INTEGRITY VIOLATION in '%s' [0x%llx..0x%llx]\n",
                    r->name, r->va_start, r->va_end);

            if (!r->alert_only) {
                spin_unlock(&g_region_lock);
                monox_panic("Kernel integrity check FAILED: region '%s' modified!\n",
                            r->name);
            }
        }
    }
    spin_unlock(&g_region_lock);
    return violations;
}

/* -------------------------------------------------------
 * sysatpam_load_signature - add a malware signature
 * -------------------------------------------------------*/
int sysatpam_load_signature(const u8 *pattern, const u8 *mask,
                              u32 len, const char *name)
{
    if (!pattern || !len || len > MAX_SIG_LEN || !name) return MX_EINVAL;

    spin_lock(&g_sig_lock);
    if (g_nr_sigs >= MAX_SIGNATURES) {
        spin_unlock(&g_sig_lock);
        return MX_ENOSPC;
    }

    struct malware_sig *sig = NULL;
    for (u32 i = 0; i < MAX_SIGNATURES; i++) {
        if (!g_signatures[i].valid) { sig = &g_signatures[i]; break; }
    }
    if (!sig) { spin_unlock(&g_sig_lock); return MX_ENOSPC; }

    memcpy(sig->pattern, pattern, len);
    if (mask)
        memcpy(sig->mask, mask, len);
    else
        memset(sig->mask, 0xFF, len);

    sig->len   = len;
    sig->valid = true;
    strncpy(sig->name, name, 63);
    g_nr_sigs++;
    spin_unlock(&g_sig_lock);
    return MX_OK;
}

/* -------------------------------------------------------
 * sysatpam_scan_memory - scan a buffer for malware signatures
 * -------------------------------------------------------*/
sysatpam_result_t sysatpam_scan_memory(const void *buf, size_t len,
                                        char *found_name, size_t name_sz)
{
    if (!buf || !len) return SYSATPAM_CLEAN;

    const u8 *data = (const u8 *)buf;

    spin_lock(&g_sig_lock);
    for (u32 i = 0; i < MAX_SIGNATURES; i++) {
        if (!g_signatures[i].valid) continue;
        const struct malware_sig *sig = &g_signatures[i];

        for (size_t off = 0; off + sig->len <= len; off++) {
            bool match = true;
            for (u32 j = 0; j < sig->len; j++) {
                if ((data[off + j] & sig->mask[j]) !=
                    (sig->pattern[j] & sig->mask[j])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                pr_warn("SYSATPAM: malware signature '%s' at offset %zu\n",
                        sig->name, off);
                /* name_sz == 0 used to become strncpy(.., SIZE_MAX): overflow */
                if (found_name && name_sz) {
                    strncpy(found_name, sig->name, name_sz - 1);
                    found_name[name_sz - 1] = '\0';
                }
                spin_unlock(&g_sig_lock);
                return SYSATPAM_INFECTED;
            }
        }
    }
    spin_unlock(&g_sig_lock);
    return SYSATPAM_CLEAN;
}

/* -------------------------------------------------------
 * sysatpam_quarantine - ban a process
 * -------------------------------------------------------*/
int sysatpam_quarantine(pid_t pid, const char *name, const char *reason)
{
    spin_lock(&g_qlock);
    for (u32 i = 0; i < MAX_QUARANTINE; i++) {
        if (!g_quarantine[i].valid) {
            g_quarantine[i].pid   = pid;
            g_quarantine[i].time  = 0; /* filled by timer */
            g_quarantine[i].valid = true;
            g_nr_quarantined++;
            if (name)   strncpy(g_quarantine[i].name,   name,   63);
            if (reason) strncpy(g_quarantine[i].reason, reason, 127);
            spin_unlock(&g_qlock);
            pr_warn("SYSATPAM: QUARANTINE pid=%u name='%s' reason='%s'\n",
                    pid, name ? name : "?", reason ? reason : "?");
            return MX_OK;
        }
    }
    spin_unlock(&g_qlock);
    return MX_ENOSPC;
}

/* -------------------------------------------------------
 * sysatpam_is_quarantined - check if pid is banned
 * -------------------------------------------------------*/
bool sysatpam_is_quarantined(pid_t pid)
{
    spin_lock(&g_qlock);
    for (u32 i = 0; i < MAX_QUARANTINE; i++) {
        if (g_quarantine[i].valid && g_quarantine[i].pid == pid) {
            spin_unlock(&g_qlock);
            return true;
        }
    }
    spin_unlock(&g_qlock);
    return false;
}

/* -------------------------------------------------------
 * Syscall argument validator
 * Called from syscall dispatch before executing any syscall
 * -------------------------------------------------------*/
int sysatpam_validate_syscall(u32 syscall_nr, u64 *args, u32 nr_args,
                               pid_t caller_pid)
{
    /* If process is quarantined, deny all syscalls */
    if (sysatpam_is_quarantined(caller_pid)) {
        pr_warn("SYSATPAM: syscall %u denied for quarantined pid=%u\n",
                syscall_nr, caller_pid);
        return MX_EPERM;
    }

    if (nr_args && !args) return MX_EINVAL;     /* args[] would be dereferenced */

    /* Validate pointer arguments are in userspace range */
    for (u32 i = 0; i < nr_args; i++) {
        u64 a = args[i];
        if (a == 0) continue;
        /* Small negative scalars (-1 fd, -errno...) are not pointers */
        if (a >= (u64)-4096) continue;
        /* Kernel VA range: reject if pointer points to kernel space */
        if (a >= 0xFFFF000000000000ULL) {
            pr_warn("SYSATPAM: syscall %u arg%u is kernel pointer 0x%llx pid=%u\n",
                    syscall_nr, i, a, caller_pid);
            return MX_EFAULT;
        }
    }

    return MX_OK;
}

/* -------------------------------------------------------
 * sysatpam_stats
 * -------------------------------------------------------*/
void sysatpam_stats(void)
{
    pr_info("SYSATPAM: regions=%u signatures=%u quarantined=%u\n",
            g_nr_regions, g_nr_sigs, g_nr_quarantined);   /* was printing g_nr_regions twice */
}
