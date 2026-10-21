/* MONOX Kernel - System Satpam Interface */

#ifndef _MONOX_SYSTEM_SATPAM_H
#define _MONOX_SYSTEM_SATPAM_H

#include <kernel/types.h>

typedef enum {
    SYSATPAM_CLEAN    = 0,
    SYSATPAM_INFECTED = 1,
    SYSATPAM_SUSPECT  = 2,
} sysatpam_result_t;

void              sysatpam_init(void);
int               sysatpam_protect_region(u64 va_start, u64 va_end,
                                           const char *name, bool alert_only);
int               sysatpam_verify_integrity(void);
int               sysatpam_load_signature(const u8 *pattern, const u8 *mask,
                                           u32 len, const char *name);
sysatpam_result_t sysatpam_scan_memory(const void *buf, size_t len,
                                        char *found_name, size_t name_sz);
int               sysatpam_quarantine(pid_t pid, const char *name,
                                       const char *reason);
bool              sysatpam_is_quarantined(pid_t pid);
int               sysatpam_validate_syscall(u32 syscall_nr, u64 *args,
                                             u32 nr_args, pid_t caller_pid);
void              sysatpam_stats(void);

/* Periodic check (call from timer or idle task) */
static inline void sysatpam_periodic_check(void) {
    sysatpam_verify_integrity();
}

#endif /* _MONOX_SYSTEM_SATPAM_H */
