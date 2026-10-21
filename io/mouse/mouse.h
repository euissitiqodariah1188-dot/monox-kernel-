/* MONOX Kernel - Mouse Interface */

#ifndef _MONOX_MOUSE_H
#define _MONOX_MOUSE_H

#include <kernel/types.h>

struct mouse_event {
    u64  timestamp_ns;
    s32  dx, dy;
    s32  abs_x, abs_y;
    s8   wheel;
    s8   pan;
    u8   buttons;
    u8   btn_changed;
};

struct mouse_state {
    u8   buttons;
    s32  abs_x, abs_y;
    bool active;
};

int  mouse_probe(u32 usb_dev_id, u8 interface, u8 endpoint,
                  bool has_report_id, u8 report_id, u8 report_len);
void mouse_disconnect(u32 usb_dev_id);
int  mouse_read_event(u32 mouse_idx, struct mouse_event *ev);
void mouse_get_state(u32 mouse_idx, struct mouse_state *st);
u32  mouse_count(void);

/* Button bit masks */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04
#define MOUSE_BTN_SIDE1  0x08
#define MOUSE_BTN_SIDE2  0x10

#endif /* _MONOX_MOUSE_H */
