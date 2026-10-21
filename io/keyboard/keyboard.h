/* MONOX Kernel - Keyboard Interface */

#ifndef _MONOX_KEYBOARD_H
#define _MONOX_KEYBOARD_H

#include <kernel/types.h>

/* Special key codes */
#define KEY_SPECIAL_NONE 0
#define KEY_UP           1
#define KEY_DOWN         2
#define KEY_LEFT         3
#define KEY_RIGHT        4
#define KEY_HOME         5
#define KEY_END          6
#define KEY_PGUP         7
#define KEY_PGDN         8
#define KEY_INSERT       9
#define KEY_DELETE       10
#define KEY_F1           11
#define KEY_F2           12
#define KEY_F3           13
#define KEY_F4           14
#define KEY_F5           15
#define KEY_F6           16
#define KEY_F7           17
#define KEY_F8           18
#define KEY_F9           19
#define KEY_F10          20
#define KEY_F11          21
#define KEY_F12          22

/* Modifier bits */
#define MOD_LCTRL   0x01
#define MOD_LSHIFT  0x02
#define MOD_LALT    0x04
#define MOD_LMETA   0x08
#define MOD_RCTRL   0x10
#define MOD_RSHIFT  0x20
#define MOD_RALT    0x40
#define MOD_RMETA   0x80

struct key_event {
    u64  timestamp_ns;
    u8   keycode;
    u8   ascii;
    u8   modifiers;
    bool pressed;
    u8   special;
};

int  kbd_probe(u32 usb_dev_id, u8 interface, u8 ep_in, u8 ep_out);
void kbd_disconnect(u32 usb_dev_id);
int  kbd_read_event(u32 kbd_idx, struct key_event *ev);
int  kbd_set_layout(u32 kbd_idx, const u8 *map_base, const u8 *map_shift);
u32  kbd_count(void);

#endif /* _MONOX_KEYBOARD_H */
