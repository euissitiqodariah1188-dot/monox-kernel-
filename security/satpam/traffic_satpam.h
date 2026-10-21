/* MONOX Kernel - Traffic Satpam Interface */

#ifndef _MONOX_TRAFFIC_SATPAM_H
#define _MONOX_TRAFFIC_SATPAM_H

#include <kernel/types.h>

typedef enum {
    SATPAM_ALLOW           = 0,
    SATPAM_DENY            = 1,
    SATPAM_INJECT_DETECTED = 2,
} satpam_result_t;

void            satpam_init(void);
satpam_result_t satpam_check_route(const char *subject, const char *target,
                                    pid_t caller_pid);
satpam_result_t satpam_scan_buffer(const void *buf, size_t len,
                                    pid_t caller_pid);
satpam_result_t satpam_check_firmware_access(pid_t caller_pid,
                                              const char *region);
int             satpam_register_trusted(pid_t pid, const char *name);
int             satpam_add_rule(pid_t caller_pid, const char *subject,
                                 const char *target, bool allow);
int             satpam_remove_rule(pid_t caller_pid, u32 rule_id);
void            satpam_stats(void);

#endif /* _MONOX_TRAFFIC_SATPAM_H */
