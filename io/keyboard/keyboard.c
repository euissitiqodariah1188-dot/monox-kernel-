/* MONOX Kernel - USB HID Keyboard Driver
 *
 * Supports: USB-C, USB 3.0, USB 3.2 keyboards
 * Protocol: USB HID Boot Protocol (6KRO) + N-KRO via report protocol
 * Features: - Real-time scancode-to-keycode translation
 *           - Multiple keyboard layouts (configurable, not hardcoded)
 *           - Modifier key handling
 *           - LED control (CapsLock, NumLock, ScrollLock)
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "keyboard.h"
#include "../../drivers/usb/usb.h"

/* -------------------------------------------------------
 * USB HID Keyboard boot report (8 bytes)
 * -------------------------------------------------------*/
struct hid_kbd_report {
    u8 modifiers;   /* bit0=LCtrl 1=LShift 2=LAlt 3=LMeta 4=RCtrl 5=RShift 6=RAlt 7=RMeta */
    u8 reserved;
    u8 keycodes[6]; /* up to 6 simultaneous keys (KRO) */
} __packed;

/* -------------------------------------------------------
 * Keycode table (HID Usage to ASCII / keycode)
 * This is data - not logic - and can be swapped for other layouts
 * -------------------------------------------------------*/
#define HID_KEY_MAX  256

/* HID keycode → ASCII (unshifted, US QWERTY default) */
static const u8 hid_to_ascii_base[HID_KEY_MAX] = {
    [0x04]='a', [0x05]='b', [0x06]='c', [0x07]='d',
    [0x08]='e', [0x09]='f', [0x0A]='g', [0x0B]='h',
    [0x0C]='i', [0x0D]='j', [0x0E]='k', [0x0F]='l',
    [0x10]='m', [0x11]='n', [0x12]='o', [0x13]='p',
    [0x14]='q', [0x15]='r', [0x16]='s', [0x17]='t',
    [0x18]='u', [0x19]='v', [0x1A]='w', [0x1B]='x',
    [0x1C]='y', [0x1D]='z',
    [0x1E]='1', [0x1F]='2', [0x20]='3', [0x21]='4',
    [0x22]='5', [0x23]='6', [0x24]='7', [0x25]='8',
    [0x26]='9', [0x27]='0',
    [0x28]='\n',[0x29]=27,  [0x2A]='\b',[0x2B]='\t',
    [0x2C]=' ', [0x2D]='-', [0x2E]='=', [0x2F]='[',
    [0x30]=']', [0x31]='\\', [0x33]=';',[0x34]='\'',
    [0x35]='`', [0x36]=',', [0x37]='.', [0x38]='/',
    [0x4F]=0,   [0x50]=0,   [0x51]=0,   [0x52]=0,  /* arrows: special */
};

/* Shifted variants */
static const u8 hid_to_ascii_shift[HID_KEY_MAX] = {
    [0x04]='A', [0x05]='B', [0x06]='C', [0x07]='D',
    [0x08]='E', [0x09]='F', [0x0A]='G', [0x0B]='H',
    [0x0C]='I', [0x0D]='J', [0x0E]='K', [0x0F]='L',
    [0x10]='M', [0x11]='N', [0x12]='O', [0x13]='P',
    [0x14]='Q', [0x15]='R', [0x16]='S', [0x17]='T',
    [0x18]='U', [0x19]='V', [0x1A]='W', [0x1B]='X',
    [0x1C]='Y', [0x1D]='Z',
    [0x1E]='!', [0x1F]='@', [0x20]='#', [0x21]='$',
    [0x22]='%', [0x23]='^', [0x24]='&', [0x25]='*',
    [0x26]='(', [0x27]=')',
    [0x28]='\n',[0x29]=27,  [0x2A]='\b',[0x2B]='\t',
    [0x2C]=' ', [0x2D]='_', [0x2E]='+', [0x2F]='{',
    [0x30]='}', [0x31]='|', [0x33]=':', [0x34]='"',
    [0x35]='~', [0x36]='<', [0x37]='>', [0x38]='?',
};

/* -------------------------------------------------------
 * Key event ring buffer (real-time, wait-free)
 * -------------------------------------------------------*/
#define KBD_RING_SZ 512

/* struct key_event comes from keyboard.h (redefinition removed) */

struct kbd_ring {
    struct key_event events[KBD_RING_SZ];
    volatile u32     head;
    volatile u32     tail;
};

/* -------------------------------------------------------
 * Keyboard device descriptor
 * -------------------------------------------------------*/
#define MAX_KEYBOARDS 4

struct kbd_dev {
    u32  usb_device_id;
    u8   interface;
    u8   endpoint_in;
    u8   endpoint_out;
    bool active;

    /* State */
    u8   modifiers;
    u8   prev_keys[6];
    bool caps_lock;
    bool num_lock;
    bool scroll_lock;
    u8   led_state;

    /* Layout (pointer to translation tables - swappable) */
    const u8 *map_base;
    const u8 *map_shift;

    /* Ring */
    struct kbd_ring ring;
    spinlock_t      lock;

    /* Stats */
    u64 event_count;
};

static struct kbd_dev g_kbds[MAX_KEYBOARDS];
static u32            g_nr_kbds = 0;
static spinlock_t     g_kbd_lock = SPIN_LOCK_INIT;

/* -------------------------------------------------------
 * Ring helpers
 * -------------------------------------------------------*/
static bool kbd_ring_push(struct kbd_ring *r, const struct key_event *e)
{
    u32 next = (READ_ONCE(r->head) + 1) & (KBD_RING_SZ - 1);
    if (next == READ_ONCE(r->tail)) return false;
    r->events[r->head] = *e;
    wmb();
    WRITE_ONCE(r->head, next);
    return true;
}

static bool kbd_ring_pop(struct kbd_ring *r, struct key_event *e)
{
    if (READ_ONCE(r->tail) == READ_ONCE(r->head)) return false;
    rmb();
    *e = r->events[r->tail];
    WRITE_ONCE(r->tail, (r->tail + 1) & (KBD_RING_SZ - 1));
    return true;
}

/* -------------------------------------------------------
 * LED update via USB HID SET_REPORT
 * -------------------------------------------------------*/
static void kbd_update_leds(struct kbd_dev *k)
{
    u8 led = 0;
    if (k->num_lock)    led |= 0x01;
    if (k->caps_lock)   led |= 0x02;
    if (k->scroll_lock) led |= 0x04;
    k->led_state = led;

    /* Send HID Output Report (ID=0, 1 byte) */
    usb_hid_set_report(k->usb_device_id, k->interface, 0x02, 0, &led, 1);
}

/* -------------------------------------------------------
 * Process one HID report (real-time callback from USB)
 * -------------------------------------------------------*/
static void kbd_usb_report(u32 dev_id, const u8 *data, u32 len, void *ctx)
{
    struct kbd_dev *k = (struct kbd_dev *)ctx;
    if (!k->active || !data || len < 8) return;

    const struct hid_kbd_report *r = (const struct hid_kbd_report *)data;
    u8 new_mod = r->modifiers;

    /* Find pressed keys (in new report, not in old) */
    for (u32 i = 0; i < 6; i++) {
        u8 kc = r->keycodes[i];
        if (!kc || kc == 0x01) continue; /* 0x01 = rollover error */

        bool was_down = false;
        for (u32 j = 0; j < 6; j++) {
            if (k->prev_keys[j] == kc) { was_down = true; break; }
        }
        if (was_down) continue;

        /* Toggle lock keys */
        if (kc == 0x39) { k->caps_lock   = !k->caps_lock;   kbd_update_leds(k); continue; }
        if (kc == 0x53) { k->num_lock    = !k->num_lock;    kbd_update_leds(k); continue; }
        if (kc == 0x47) { k->scroll_lock = !k->scroll_lock; kbd_update_leds(k); continue; }

        bool shift = (new_mod & 0x22) != 0;  /* L/R shift */
        bool effective_caps = k->caps_lock ^ shift;  /* caps XOR shift for alpha */

        u8 ascii = 0;
        if (kc >= 0x04 && kc <= 0x1D) {
            /* Alpha key */
            ascii = effective_caps ? k->map_shift[kc] : k->map_base[kc];
        } else {
            ascii = shift ? k->map_shift[kc] : k->map_base[kc];
        }

        u8 special = KEY_SPECIAL_NONE;
        if      (kc == 0x4F) special = KEY_RIGHT;
        else if (kc == 0x50) special = KEY_LEFT;
        else if (kc == 0x51) special = KEY_DOWN;
        else if (kc == 0x52) special = KEY_UP;
        else if (kc == 0x4A) special = KEY_HOME;
        else if (kc == 0x4D) special = KEY_END;
        else if (kc == 0x4B) special = KEY_PGUP;
        else if (kc == 0x4E) special = KEY_PGDN;
        else if (kc == 0x49) special = KEY_INSERT;
        else if (kc == 0x4C) special = KEY_DELETE;
        else if (kc >= 0x3A && kc <= 0x45) special = KEY_F1 + (kc - 0x3A);

        struct key_event ev = {
            .keycode   = kc,
            .ascii     = ascii,
            .modifiers = new_mod,
            .pressed   = true,
            .special   = special,
        };
        kbd_ring_push(&k->ring, &ev);
        k->event_count++;
    }

    /* Find released keys (in old report, not in new) */
    for (u32 i = 0; i < 6; i++) {
        u8 kc = k->prev_keys[i];
        if (!kc) continue;
        bool still_down = false;
        for (u32 j = 0; j < 6; j++) {
            if (r->keycodes[j] == kc) { still_down = true; break; }
        }
        if (!still_down) {
            struct key_event ev = {
                .keycode   = kc,
                .ascii     = 0,
                .modifiers = new_mod,
                .pressed   = false,
            };
            kbd_ring_push(&k->ring, &ev);
        }
    }

    k->modifiers = new_mod;
    memcpy(k->prev_keys, r->keycodes, 6);
}

/* -------------------------------------------------------
 * kbd_probe - register a USB keyboard
 * -------------------------------------------------------*/
int kbd_probe(u32 usb_dev_id, u8 interface,
               u8 ep_in, u8 ep_out)
{
    u64 irqf;
    spin_lock_irqsave(&g_kbd_lock, irqf);

    /* reuse slots of disconnected keyboards (they used to leak forever) */
    u32 idx = g_nr_kbds;
    for (u32 i = 0; i < g_nr_kbds; i++) {
        if (!g_kbds[i].active) { idx = i; break; }
    }
    if (idx >= MAX_KEYBOARDS) {
        spin_unlock_irqrestore(&g_kbd_lock, irqf);
        return MX_ENOSPC;
    }

    struct kbd_dev *k = &g_kbds[idx];
    memset(k, 0, sizeof(*k));
    spin_lock_init(&k->lock);

    k->usb_device_id = usb_dev_id;
    k->interface     = interface;
    k->endpoint_in   = ep_in;
    k->endpoint_out  = ep_out;
    k->map_base      = hid_to_ascii_base;
    k->map_shift     = hid_to_ascii_shift;
    k->active        = true;
    k->num_lock      = true;  /* NumLock default ON */

    int ret = usb_hid_register_interrupt(usb_dev_id, interface, ep_in,
                                          8, kbd_usb_report, k);
    if (ret != MX_OK) {
        k->active = false;               /* leave the slot free */
        spin_unlock_irqrestore(&g_kbd_lock, irqf);
        return ret;
    }

    kbd_update_leds(k);
    if (idx == g_nr_kbds) g_nr_kbds++;
    spin_unlock_irqrestore(&g_kbd_lock, irqf);

    pr_info("KBD[%u]: USB keyboard dev=%u iface=%u ep_in=0x%02x ep_out=0x%02x\n",
            idx, usb_dev_id, interface, ep_in, ep_out);
    return MX_OK;
}

/* -------------------------------------------------------
 * kbd_disconnect
 * -------------------------------------------------------*/
void kbd_disconnect(u32 usb_dev_id)
{
    u64 irqf;
    spin_lock_irqsave(&g_kbd_lock, irqf);
    for (u32 i = 0; i < g_nr_kbds; i++) {
        if (g_kbds[i].active && g_kbds[i].usb_device_id == usb_dev_id) {
            g_kbds[i].active = false;
            pr_info("KBD[%u]: disconnected\n", i);
        }
    }
    spin_unlock_irqrestore(&g_kbd_lock, irqf);
}

/* -------------------------------------------------------
 * kbd_read_event - polling consumer API
 * -------------------------------------------------------*/
int kbd_read_event(u32 kbd_idx, struct key_event *ev)
{
    if (kbd_idx >= g_nr_kbds || !ev) return MX_EINVAL;
    return kbd_ring_pop(&g_kbds[kbd_idx].ring, ev) ? MX_OK : MX_EAGAIN;
}

/* -------------------------------------------------------
 * kbd_set_layout - swap translation tables at runtime
 * -------------------------------------------------------*/
int kbd_set_layout(u32 kbd_idx, const u8 *map_base, const u8 *map_shift)
{
    if (kbd_idx >= g_nr_kbds) return MX_EINVAL;
    if (!map_base || !map_shift) return MX_EINVAL;
    g_kbds[kbd_idx].map_base  = map_base;
    g_kbds[kbd_idx].map_shift = map_shift;
    return MX_OK;
}

u32 kbd_count(void) { return g_nr_kbds; }
