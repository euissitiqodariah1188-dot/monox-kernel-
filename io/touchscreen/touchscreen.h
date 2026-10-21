/* MONOX Kernel - Touchscreen Interface */

#ifndef _MONOX_TOUCHSCREEN_H
#define _MONOX_TOUCHSCREEN_H

#include <kernel/types.h>

#define TS_EV_TOUCH  0
#define TS_EV_LIFT   1
#define TS_EV_MOVE   2

struct ts_event {
    u32  type;
    s32  x, y;
    s32  pressure;
    s32  slot;
    s32  tracking_id;
    u64  timestamp_ns;
};

int  ts_probe(const char **dtb_props);
int  ts_read_event(u32 device_idx, struct ts_event *ev);
void ts_enable(u32 idx);
void ts_disable(u32 idx);
void ts_suspend(u32 idx);
void ts_resume(u32 idx);

#endif /* _MONOX_TOUCHSCREEN_H */
