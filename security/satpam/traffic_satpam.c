/* MONOX Kernel - Traffic Satpam (Traffic Security Guard)
 *
 * Responsibilities:
 *  1. Control which software can access which hardware path
 *  2. Scan for code injection / shellcode patterns
 *  3. Prevent software from accessing BIOS/UEFI directly
 *     (only kernel + trusted system components may access firmware)
 *  4. Enforce software-to-hardware routing rules
 *  5. Log all policy violations
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "traffic_satpam.h"

/* -------------------------------------------------------
 * Routing rule table (data-driven, no hardcoded paths)
 * -------------------------------------------------------*/
#define MAX_RULES         256
#define MAX_TRUSTED_PIDS   64

struct routing_rule {
    u32         rule_id;
    char        subject[64];    /* caller identifier / driver name  */
    char        target[64];     /* target hw path / device class    */
    bool        allow;
    bool        log;
    bool        valid;
};

struct trusted_entry {
    pid_t       pid;
    char        name[64];
    bool        valid;
};

static struct routing_rule g_rules[MAX_RULES];
static u32                 g_nr_rules = 0;
static spinlock_t          g_rule_lock = SPIN_LOCK_INIT;

static struct trusted_entry g_trusted[MAX_TRUSTED_PIDS];
static u32                  g_nr_trusted = 0;
static spinlock_t           g_trusted_lock = SPIN_LOCK_INIT;

/* Policy counters */
static atomic64_t g_allow_count  = ATOMIC64_INIT(0);
static atomic64_t g_deny_count   = ATOMIC64_INIT(0);
static atomic64_t g_inject_count = ATOMIC64_INIT(0);

/* -------------------------------------------------------
 * Built-in default rules (loaded at init, configurable)
 * -------------------------------------------------------*/
static const struct {
    const char *subject;
    const char *target;
    bool        allow;
} DEFAULT_RULES[] = {
    /* Kernel internal: full access */
    { "kernel",          "*",               true  },
    /* Kernel + init allowed to firmware. These MUST come before the
     * wildcard denies below: rules are first-match-wins, so in the old order
     * ("*" deny first) the "overrides wildcard above" entries were
     * unreachable and monox_init was denied firmware.uefi. */
    { "monox_init",      "firmware.uefi",   true  },
    /* Trusted system services: hardware access allowed */
    { "monox_init",      "hardware.*",      true  },
    { "gpu_driver",      "hardware.gpu",    true  },
    { "cpu_driver",      "hardware.cpu",    true  },
    { "usb_driver",      "hardware.usb",    true  },
    { "net_driver",      "hardware.net",    true  },
    { "storage_driver",  "hardware.storage",true  },
    /* ALL other software: block BIOS/UEFI access */
    { "*",               "firmware.bios",   false },
    { "*",               "firmware.uefi",   false },
    { "*",               "firmware.smc",    false },
    /* User-space: no raw hardware access */
    { "userspace.*",     "hardware.*",      false },
    /* User-space: allowed to use driver APIs */
    { "userspace.*",     "driver.api.*",    true  },
};

/* -------------------------------------------------------
 * Known shellcode / injection byte signatures
 * (ARM64 instruction patterns used in ROP/code injection)
 * -------------------------------------------------------*/
static const u8 SUSPICIOUS_PATTERNS[][8] = {
    /* SMC #0 (0xD4000003). The old pattern 01 00 00 D4 is SVC #0 - the
     * normal system-call instruction present in every user program, which
     * made every legitimate binary "INJECT_DETECTED". */
    { 0x03, 0x00, 0x00, 0xD4 },  /* smc #0 */
    /* HVC in EL0 context */
    { 0x02, 0x00, 0x00, 0xD4 },  /* hvc #0 */
    /* Attempt to write VBAR directly from non-kernel */
    { 0x08, 0xC0, 0x18, 0xD5 },  /* msr vbar_el1, x8 */
    /* Overwrite SCTLR (disable MMU) */
    { 0x00, 0x10, 0x18, 0xD5 },  /* msr sctlr_el1, x0 */
};
#define NR_SUSPICIOUS (sizeof(SUSPICIOUS_PATTERNS) / sizeof(SUSPICIOUS_PATTERNS[0]))

/* -------------------------------------------------------
 * satpam_init - load default rules and init
 * -------------------------------------------------------*/
void satpam_init(void)
{
    spin_lock_init(&g_rule_lock);
    spin_lock_init(&g_trusted_lock);
    memset(g_rules,   0, sizeof(g_rules));
    memset(g_trusted, 0, sizeof(g_trusted));

    /* Load default rules */
    u32 nr = sizeof(DEFAULT_RULES) / sizeof(DEFAULT_RULES[0]);
    for (u32 i = 0; i < nr && i < MAX_RULES; i++) {
        strncpy(g_rules[i].subject, DEFAULT_RULES[i].subject, 63);
        strncpy(g_rules[i].target,  DEFAULT_RULES[i].target,  63);
        g_rules[i].allow   = DEFAULT_RULES[i].allow;
        g_rules[i].log     = true;
        g_rules[i].valid   = true;
        g_rules[i].rule_id = i;
    }
    g_nr_rules = nr;

    /* Register kernel as trusted */
    satpam_register_trusted(0, "kernel");
    satpam_register_trusted(1, "monox_init");

    pr_info("SATPAM: Traffic guard online (%u rules)\n", g_nr_rules);
}

/* -------------------------------------------------------
 * Pattern matching helpers
 * -------------------------------------------------------*/
static bool wildcard_match(const char *pattern, const char *str)
{
    if (strcmp(pattern, "*") == 0) return true;
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;
            while (*str) {
                if (wildcard_match(pattern, str)) return true;
                str++;
            }
            return false;
        }
        if (*pattern != *str) return false;
        pattern++;
        str++;
    }
    return (*pattern == '\0' || strcmp(pattern, "*") == 0) && *str == '\0';
}

/* -------------------------------------------------------
 * satpam_check_route - verify if subject may access target
 * -------------------------------------------------------*/
satpam_result_t satpam_check_route(const char *subject, const char *target,
                                    pid_t caller_pid)
{
    if (!subject || !target) return SATPAM_DENY;

    spin_lock(&g_rule_lock);

    /* First match wins (rules are ordered: specific before wildcard) */
    /* scan ALL slots: after satpam_remove_rule() the valid rules are no
     * longer contiguous and g_nr_rules is only a count, so the old
     * 'i < g_nr_rules' bound silently skipped the last valid rules */
    for (u32 i = 0; i < MAX_RULES; i++) {
        if (!g_rules[i].valid) continue;

        bool subj_match = wildcard_match(g_rules[i].subject, subject);
        bool tgt_match  = wildcard_match(g_rules[i].target,  target);

        if (!subj_match || !tgt_match) continue;

        bool allowed = g_rules[i].allow;
        bool do_log  = g_rules[i].log;
        spin_unlock(&g_rule_lock);

        if (do_log && !allowed) {
            pr_warn("SATPAM: DENY  pid=%u subject='%s' target='%s'\n",
                    caller_pid, subject, target);
            atomic64_add(1, &g_deny_count);
        } else if (allowed) {
            atomic64_add(1, &g_allow_count);
        }

        return allowed ? SATPAM_ALLOW : SATPAM_DENY;
    }

    spin_unlock(&g_rule_lock);

    /* Default deny */
    pr_warn("SATPAM: DEFAULT DENY pid=%u subject='%s' target='%s'\n",
            caller_pid, subject, target);
    atomic64_add(1, &g_deny_count);
    return SATPAM_DENY;
}

/* -------------------------------------------------------
 * satpam_scan_buffer - scan memory buffer for injection patterns
 * -------------------------------------------------------*/
satpam_result_t satpam_scan_buffer(const void *buf, size_t len,
                                    pid_t caller_pid)
{
    if (!buf || len < 4) return SATPAM_ALLOW;

    const u8 *data = (const u8 *)buf;

    for (size_t off = 0; off + 4 <= len; off += 4) {
        for (u32 p = 0; p < NR_SUSPICIOUS; p++) {
            if (memcmp(data + off, SUSPICIOUS_PATTERNS[p], 4) == 0) {
                pr_warn("SATPAM: INJECT pattern %u at offset %zu pid=%u\n",
                        p, off, caller_pid);
                atomic64_add(1, &g_inject_count);
                return SATPAM_INJECT_DETECTED;
            }
        }
    }

    return SATPAM_ALLOW;
}

/* -------------------------------------------------------
 * satpam_check_firmware_access
 * Only kernel (PID 0 / known trusted PIDs) may touch firmware
 * -------------------------------------------------------*/
satpam_result_t satpam_check_firmware_access(pid_t caller_pid,
                                              const char *region)
{
    spin_lock(&g_trusted_lock);
    for (u32 i = 0; i < MAX_TRUSTED_PIDS; i++) {
        if (g_trusted[i].valid && g_trusted[i].pid == caller_pid) {
            spin_unlock(&g_trusted_lock);
            return SATPAM_ALLOW;
        }
    }
    spin_unlock(&g_trusted_lock);

    pr_warn("SATPAM: FIRMWARE ACCESS DENIED pid=%u region='%s'\n",
            caller_pid, region ? region : "(null)");
    atomic64_add(1, &g_deny_count);
    return SATPAM_DENY;
}

/* -------------------------------------------------------
 * satpam_register_trusted - register a trusted PID
 * (called only from kernel init code)
 * -------------------------------------------------------*/
int satpam_register_trusted(pid_t pid, const char *name)
{
    if (!name) return MX_EINVAL;
    spin_lock(&g_trusted_lock);
    if (g_nr_trusted >= MAX_TRUSTED_PIDS) {
        spin_unlock(&g_trusted_lock);
        return MX_ENOSPC;
    }
    for (u32 i = 0; i < MAX_TRUSTED_PIDS; i++) {
        if (!g_trusted[i].valid) {
            g_trusted[i].pid   = pid;
            g_trusted[i].valid = true;
            strncpy(g_trusted[i].name, name, 63);
            g_nr_trusted++;
            spin_unlock(&g_trusted_lock);
            return MX_OK;
        }
    }
    spin_unlock(&g_trusted_lock);
    return MX_ENOSPC;
}

/* -------------------------------------------------------
 * satpam_add_rule - add a routing rule at runtime
 * (requires caller to be trusted)
 * -------------------------------------------------------*/
int satpam_add_rule(pid_t caller_pid, const char *subject,
                    const char *target, bool allow)
{
    if (!subject || !target) return MX_EINVAL;
    if (satpam_check_firmware_access(caller_pid, "satpam.rules") != SATPAM_ALLOW)
        return MX_EPERM;

    spin_lock(&g_rule_lock);
    if (g_nr_rules >= MAX_RULES) {
        spin_unlock(&g_rule_lock);
        return MX_ENOSPC;
    }
    for (u32 i = 0; i < MAX_RULES; i++) {
        if (!g_rules[i].valid) {
            strncpy(g_rules[i].subject, subject, 63);
            strncpy(g_rules[i].target,  target,  63);
            g_rules[i].allow   = allow;
            g_rules[i].log     = true;
            g_rules[i].valid   = true;
            g_rules[i].rule_id = i;
            g_nr_rules++;
            spin_unlock(&g_rule_lock);
            pr_info("SATPAM: rule added [%s] %s → %s\n",
                    allow ? "ALLOW" : "DENY", subject, target);
            return MX_OK;
        }
    }
    spin_unlock(&g_rule_lock);
    return MX_ENOSPC;
}

/* -------------------------------------------------------
 * satpam_remove_rule
 * -------------------------------------------------------*/
int satpam_remove_rule(pid_t caller_pid, u32 rule_id)
{
    if (satpam_check_firmware_access(caller_pid, "satpam.rules") != SATPAM_ALLOW)
        return MX_EPERM;

    spin_lock(&g_rule_lock);
    for (u32 i = 0; i < MAX_RULES; i++) {
        if (g_rules[i].valid && g_rules[i].rule_id == rule_id) {
            memset(&g_rules[i], 0, sizeof(g_rules[i]));
            g_nr_rules--;
            spin_unlock(&g_rule_lock);
            return MX_OK;
        }
    }
    spin_unlock(&g_rule_lock);
    return MX_ENOENT;
}

/* -------------------------------------------------------
 * satpam_stats
 * -------------------------------------------------------*/
void satpam_stats(void)
{
    pr_info("SATPAM: allow=%lld deny=%lld inject=%lld rules=%u trusted=%u\n",
            atomic64_read(&g_allow_count),
            atomic64_read(&g_deny_count),
            atomic64_read(&g_inject_count),
            g_nr_rules, g_nr_trusted);
}
