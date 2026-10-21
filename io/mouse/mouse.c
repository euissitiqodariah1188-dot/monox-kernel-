/* MONOX Kernel - USB HID Mouse Driver
 *
 * Supports: USB-C, USB 3.0, USB 3.2 attached mice
 * Protocol: USB HID Boot Protocol + Report Protocol
 * Provides: absolute/relative movement, scroll, buttons
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "mouse.h"
#include "../../drivers/usb/usb.h"

/* -------------------------------------------------------
 * HID Mouse report (boot protocol: 3 bytes min)
 * Extended: up to 8 bytes with scroll + extra buttons
 * -------------------------------------------------------*/
struct hid_mouse_report {
    u8  buttons;     /* bit0=left bit1=right bit2=middle */
    s8  dx;          /* X delta                          */
    s8  dy;          /* Y delta                          */
    s8  wheel;       /* scroll wheel                     */
    s8  pan;         /* horizontal scroll (report proto) */
    u8  extra_btn;   /* buttons 4-8                      */
} __packed;

/* -------------------------------------------------------
 * Mouse event ring buffer
 * -------------------------------------------------------*/
#define MOUSE_RING_SZ  128

/* struct mouse_event comes from mouse.h (redefinition removed) */

struct mouse_ring {
    struct mouse_event events[MOUSE_RING_SZ];
    volatile u32       head;
    volatile u32       tail;
};

/* -------------------------------------------------------
 * Mouse device descriptor
 * -------------------------------------------------------*/
#define MAX_MICE 8

struct mouse_dev {
    u32              usb_device_id;
    u8               interface;
    u8               endpoint;
    bool             active;

    /* Current state */
    u8               buttons;
    s32              abs_x, abs_y;
    s32              abs_x_min, abs_x_max;
    s32              abs_y_min, abs_y_max;
    bool             has_abs;

    /* Report protocol info */
    u8               report_id;
    u8               report_len;

    /* Ring buffer */
    struct mouse_ring ring;
    spinlock_t        lock;

    /* Stats */
    u64              event_count;
    u64              irq_count;
};

static struct mouse_dev g_mice[MAX_MICE];
static u32              g_nr_mice = 0;
static spinlock_t       g_mice_lock = SPIN_LOCK_INIT;

/* -------------------------------------------------------
 * Ring helpers
 * -------------------------------------------------------*/
static bool mouse_ring_push(struct mouse_ring *r, const struct mouse_event *e)
{
    u32 next = (READ_ONCE(r->head) + 1) & (MOUSE_RING_SZ - 1);
    if (next == READ_ONCE(r->tail)) return false;
    r->events[r->head] = *e;
    wmb();
    WRITE_ONCE(r->head, next);
    return true;
}

static bool mouse_ring_pop(struct mouse_ring *r, struct mouse_event *e)
{
    if (READ_ONCE(r->tail) == READ_ONCE(r->head)) return false;
    rmb();
    *e = r->events[r->tail];
    WRITE_ONCE(r->tail, (r->tail + 1) & (MOUSE_RING_SZ - 1));
    return true;
}

/* -------------------------------------------------------
 * USB interrupt callback - called per USB HID report
 * -------------------------------------------------------*/
static void mouse_usb_report(u32 dev_id, const u8 *data, u32 len, void *ctx)
{
    struct mouse_dev *m = (struct mouse_dev *)ctx;
    if (!m->active || !data || len < 3) return;

    m->irq_count++;

    const u8 *p = data;
    /* Skip report ID byte if present (and account for it in the length:
     * the fields below were read past the end for 3-4 byte reports) */
    if (m->report_id && p[0] == m->report_id) { p++; len--; }
    if (len < 3) return;

    const struct hid_mouse_report *r = (const struct hid_mouse_report *)p;

    u8 prev_buttons = m->buttons;
    m->buttons = r->buttons;

    /* Absolute mode (tablet / pointing stick) */
    if (m->has_abs) {
        /* For abs devices, dx/dy are reinterpreted as absolute coords */
        m->abs_x = CLAMP(m->abs_x + r->dx, m->abs_x_min, m->abs_x_max);
        m->abs_y = CLAMP(m->abs_y + r->dy, m->abs_y_min, m->abs_y_max);
    }

    struct mouse_event ev = {
        .timestamp_ns = 0,  /* filled by caller if needed */
        .dx           = r->dx,
        .dy           = r->dy,
        .abs_x        = m->abs_x,
        .abs_y        = m->abs_y,
        .wheel        = (len >= 4) ? r->wheel : 0,   /* byte 3 only exists for len >= 4 */
        .pan          = (len >= 5) ? r->pan : 0,
        .buttons      = r->buttons,
        .btn_changed  = r->buttons ^ prev_buttons,
    };

    mouse_ring_push(&m->ring, &ev);
    m->event_count++;
}

/* -------------------------------------------------------
 * mouse_probe - called by USB subsystem when HID mouse found
 * -------------------------------------------------------*/
int mouse_probe(u32 usb_dev_id, u8 interface, u8 endpoint,
                bool has_report_id, u8 report_id, u8 report_len)
{
    u64 irqf;
    spin_lock_irqsave(&g_mice_lock, irqf);

    /* reuse the slot of a disconnected mouse: slots used to be consumed
     * forever, so the 9th plug-in always failed with ENOSPC */
    u32 idx = g_nr_mice;
    for (u32 i = 0; i < g_nr_mice; i++) {
        if (!g_mice[i].active) { idx = i; break; }
    }
    if (idx >= MAX_MICE) {
        spin_unlock_irqrestore(&g_mice_lock, irqf);
        return MX_ENOSPC;
    }

    struct mouse_dev *m = &g_mice[idx];
    memset(m, 0, sizeof(*m));
    spin_lock_init(&m->lock);

    m->usb_device_id = usb_dev_id;
    m->interface     = interface;
    m->endpoint      = endpoint;
    m->report_id     = has_report_id ? report_id : 0;
    m->report_len    = report_len ? report_len : 4;
    m->abs_x_min     = 0;
    m->abs_x_max     = 32767;
    m->abs_y_min     = 0;
    m->abs_y_max     = 32767;
    m->active        = true;

    /* Register USB interrupt transfer callback */
    int ret = usb_hid_register_interrupt(usb_dev_id, interface, endpoint,
                                          m->report_len, mouse_usb_report, m);
    if (ret != MX_OK) {
        m->active = false;               /* leave the slot free */
        spin_unlock_irqrestore(&g_mice_lock, irqf);
        return ret;
    }

    if (idx == g_nr_mice) g_nr_mice++;
    spin_unlock_irqrestore(&g_mice_lock, irqf);

    pr_info("MOUSE[%u]: USB dev %u iface %u ep %u report_len=%u\n",
            idx, usb_dev_id, interface, endpoint, m->report_len);
    return MX_OK;
}

/* -------------------------------------------------------
 * mouse_disconnect - called when USB mouse is unplugged
 * -------------------------------------------------------*/
void mouse_disconnect(u32 usb_dev_id)
{
    u64 irqf;
    spin_lock_irqsave(&g_mice_lock, irqf);
    for (u32 i = 0; i < g_nr_mice; i++) {
        if (g_mice[i].active && g_mice[i].usb_device_id == usb_dev_id) {
            g_mice[i].active = false;
            pr_info("MOUSE[%u]: disconnected\n", i);
        }
    }
    spin_unlock_irqrestore(&g_mice_lock, irqf);
}

/* -------------------------------------------------------
 * mouse_read_event - consumer API (polling)
 * -------------------------------------------------------*/
int mouse_read_event(u32 mouse_idx, struct mouse_event *ev)
{
    if (mouse_idx >= g_nr_mice || !ev) return MX_EINVAL;
    return mouse_ring_pop(&g_mice[mouse_idx].ring, ev) ? MX_OK : MX_EAGAIN;
}

/* -------------------------------------------------------
 * mouse_count
 * -------------------------------------------------------*/
u32 mouse_count(void) { return g_nr_mice; }

/* -------------------------------------------------------
 * mouse_get_state - return current button + position state
 * -------------------------------------------------------*/
void mouse_get_state(u32 mouse_idx, struct mouse_state *st)
{
    if (mouse_idx >= g_nr_mice || !st) return;
    const struct mouse_dev *m = &g_mice[mouse_idx];
    st->buttons = m->buttons;
    st->abs_x   = m->abs_x;
    st->abs_y   = m->abs_y;
    st->active  = m->active;
}
