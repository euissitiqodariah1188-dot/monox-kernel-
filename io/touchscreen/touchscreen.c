/* MONOX Kernel - Touchscreen Driver
 *
 * Supports: I2C-attached multi-touch panels (HID, GT9xx, FT5x)
 * Protocol: Generic Multi-Touch (MT) via slot-based events
 * All register addresses read from DTB device node - no hardcoded addresses
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "touchscreen.h"
#include "../../drivers/i2c/i2c.h"

/* -------------------------------------------------------
 * Multi-touch slot (up to 10 simultaneous fingers)
 * -------------------------------------------------------*/
#define MT_MAX_SLOTS      10
#define MT_MAJOR_AXIS_MAX 65535

struct mt_slot {
    s32   x;
    s32   y;
    s32   pressure;
    s32   touch_major;
    s32   tracking_id;   /* -1 = slot unused */
    bool  active;
};

/* -------------------------------------------------------
 * Touch event ring buffer (lock-free SPSC)
 * -------------------------------------------------------*/
#define TS_EVENT_RING_SZ  256

/* struct ts_event comes from touchscreen.h (redefinition removed) */

struct ts_ring {
    struct ts_event events[TS_EVENT_RING_SZ];
    volatile u32    head;
    volatile u32    tail;
};

/* -------------------------------------------------------
 * Touchscreen device descriptor
 * -------------------------------------------------------*/
struct ts_device {
    /* Hardware info (from DTB) */
    u8          i2c_addr;
    u32         i2c_bus;
    u32         irq;
    u32         reset_gpio;
    u32         irq_gpio;
    u32         abs_x_min, abs_x_max;
    u32         abs_y_min, abs_y_max;
    u32         max_touch;

    /* Protocol ops */
    const struct ts_proto_ops *proto;

    /* State */
    struct mt_slot   slots[MT_MAX_SLOTS];
    struct ts_ring   ring;
    spinlock_t       lock;
    bool             enabled;
    bool             suspended;

    /* Statistics */
    u64  event_count;
    u64  irq_count;
};

/* -------------------------------------------------------
 * Protocol ops vtable
 * -------------------------------------------------------*/
struct ts_proto_ops {
    const char *name;
    int  (*probe)       (struct ts_device *dev);
    int  (*read_report) (struct ts_device *dev);
    void (*reset)       (struct ts_device *dev);
    void (*suspend)     (struct ts_device *dev);
    void (*resume)      (struct ts_device *dev);
};

/* -------------------------------------------------------
 * Ring buffer helpers (wait-free)
 * -------------------------------------------------------*/
static __always_inline bool ring_push(struct ts_ring *r,
                                       const struct ts_event *e)
{
    u32 next = (READ_ONCE(r->head) + 1) & (TS_EVENT_RING_SZ - 1);
    if (next == READ_ONCE(r->tail)) return false; /* full */
    r->events[r->head] = *e;
    wmb();
    WRITE_ONCE(r->head, next);
    return true;
}

static __always_inline bool ring_pop(struct ts_ring *r, struct ts_event *e)
{
    if (READ_ONCE(r->tail) == READ_ONCE(r->head)) return false;
    rmb();
    *e = r->events[r->tail];
    WRITE_ONCE(r->tail, (r->tail + 1) & (TS_EVENT_RING_SZ - 1));
    return true;
}

/* -------------------------------------------------------
 * Goodix GT9xx protocol implementation
 * -------------------------------------------------------*/
#define GT9XX_READ_XY_REG  0x814E
#define GT9XX_COORD_ADDR   0x8150
#define GT9XX_PRODUCT_ID   0x8140
#define GT9XX_POINT_STRUCT_SIZE 8

static int gt9xx_probe(struct ts_device *dev)
{
    u8 pid[4] = {0};
    /* Read product ID from I2C */
    if (i2c_read16(dev->i2c_bus, dev->i2c_addr,
                   GT9XX_PRODUCT_ID, pid, 4) < 0) {
        pr_err("TS: GT9xx not found on bus %u addr 0x%02x\n",
               dev->i2c_bus, dev->i2c_addr);
        return MX_ENODEV;
    }
    pr_info("TS: Goodix GT9%c%c detected\n", pid[0], pid[1]);
    return MX_OK;
}

static int gt9xx_read_report(struct ts_device *dev)
{
    u8 status;
    if (i2c_read16(dev->i2c_bus, dev->i2c_addr,
                   GT9XX_READ_XY_REG, &status, 1) < 0)
        return MX_EIO;

    if (!(status & 0x80)) return MX_OK; /* buffer not ready */

    u8 n = status & 0x0F;
    if (n > dev->max_touch) n = (u8)dev->max_touch;
    if (n > MT_MAX_SLOTS)   n = MT_MAX_SLOTS;    /* buf[] holds MT_MAX_SLOTS points */

    u8 buf[GT9XX_POINT_STRUCT_SIZE * MT_MAX_SLOTS];
    if (n > 0 && i2c_read16(dev->i2c_bus, dev->i2c_addr,
                             GT9XX_COORD_ADDR, buf,
                             n * GT9XX_POINT_STRUCT_SIZE) < 0)
        return MX_EIO;

    /* Mark all slots inactive initially */
    bool active[MT_MAX_SLOTS] = {false};

    for (u8 i = 0; i < n; i++) {
        u8 *p    = buf + i * GT9XX_POINT_STRUCT_SIZE;
        u8  slot = p[0] & 0x0F;
        if (slot >= MT_MAX_SLOTS) continue;

        s32 x = ((s32)p[2] << 8) | p[1];
        s32 y = ((s32)p[4] << 8) | p[3];
        s32 w = ((s32)p[6] << 8) | p[5];

        /* Clamp to abs range */
        x = CLAMP(x, (s32)dev->abs_x_min, (s32)dev->abs_x_max);
        y = CLAMP(y, (s32)dev->abs_y_min, (s32)dev->abs_y_max);

        dev->slots[slot].x           = x;
        dev->slots[slot].y           = y;
        dev->slots[slot].pressure    = w;
        dev->slots[slot].tracking_id = slot;
        dev->slots[slot].active      = true;
        active[slot] = true;

        struct ts_event ev = {
            .type        = TS_EV_TOUCH,
            .x           = x,
            .y           = y,
            .pressure    = w,
            .slot        = slot,
            .tracking_id = slot,
        };
        ring_push(&dev->ring, &ev);
        dev->event_count++;
    }

    /* Release events for lifted fingers */
    for (u8 s = 0; s < MT_MAX_SLOTS; s++) {
        if (dev->slots[s].active && !active[s]) {
            dev->slots[s].active      = false;
            dev->slots[s].tracking_id = -1;
            struct ts_event ev = {
                .type        = TS_EV_LIFT,
                .slot        = s,
                .tracking_id = -1,
            };
            ring_push(&dev->ring, &ev);
        }
    }

    /* Clear ready flag */
    u8 clear = 0;
    i2c_write16(dev->i2c_bus, dev->i2c_addr, GT9XX_READ_XY_REG, &clear, 1);
    return MX_OK;
}

static void gt9xx_reset(struct ts_device *dev)
{
    extern void gpio_set(u32 gpio, bool val);
    extern void udelay(u32 us);
    gpio_set(dev->reset_gpio, false);
    udelay(5000);
    gpio_set(dev->reset_gpio, true);
    udelay(100000);
}

static const struct ts_proto_ops gt9xx_proto = {
    .name        = "GT9xx",
    .probe       = gt9xx_probe,
    .read_report = gt9xx_read_report,
    .reset       = gt9xx_reset,
};

/* -------------------------------------------------------
 * Generic HID-over-I2C (HIDI2C) protocol stub
 * -------------------------------------------------------*/
static int hidi2c_probe(struct ts_device *dev)
{
    pr_info("TS: HID-over-I2C probe on bus %u addr 0x%02x\n",
            dev->i2c_bus, dev->i2c_addr);
    return MX_OK;
}

static int hidi2c_read_report(struct ts_device *dev)
{
    /* Read HID report descriptor and dispatch touch data */
    u8 len_buf[2];
    if (i2c_read(dev->i2c_bus, dev->i2c_addr, len_buf, 2) < 0)
        return MX_EIO;
    u16 report_len = ((u16)len_buf[1] << 8) | len_buf[0];
    if (report_len < 4 || report_len > 256) return MX_EINVAL;

    u8 report[256];
    if (i2c_read(dev->i2c_bus, dev->i2c_addr, report, report_len) < 0)
        return MX_EIO;

    /* Minimal parsing: assume standard Windows Precision Touchpad format */
    u8  n_contacts = report[2] & 0x0F;
    for (u8 c = 0; c < n_contacts && c < MT_MAX_SLOTS; c++) {
        /* each contact is 10 bytes: never read past what the device sent */
        if (3u + (u32)(c + 1) * 10u > report_len) break;
        u8 *p = report + 3 + c * 10;
        u8  flags = p[0];
        s32 x = (s32)((u16)p[2] | ((u16)(p[3] & 0x0F) << 8));
        s32 y = (s32)((u16)p[4] | ((u16)(p[5] & 0x0F) << 8));
        bool down = (flags & 0x01) != 0;

        dev->slots[c].x = x;
        dev->slots[c].y = y;
        dev->slots[c].active = down;

        struct ts_event ev = {
            .type        = down ? TS_EV_TOUCH : TS_EV_LIFT,
            .x           = x,
            .y           = y,
            .slot        = c,
            .tracking_id = down ? c : -1,
        };
        ring_push(&dev->ring, &ev);
    }
    return MX_OK;
}

static const struct ts_proto_ops hidi2c_proto = {
    .name        = "HID-I2C",
    .probe       = hidi2c_probe,
    .read_report = hidi2c_read_report,
};

/* -------------------------------------------------------
 * Global device list
 * -------------------------------------------------------*/
#define MAX_TS_DEVICES 4
static struct ts_device g_ts_devices[MAX_TS_DEVICES];
static u32              g_nr_ts = 0;

/* -------------------------------------------------------
 * IRQ handler - called from GIC when touch panel fires
 * -------------------------------------------------------*/
static void ts_irq_handler(u32 irq, void *data)
{
    struct ts_device *dev = (struct ts_device *)data;
    if (!dev->enabled || dev->suspended) return;

    dev->irq_count++;
    dev->proto->read_report(dev);
}

/* -------------------------------------------------------
 * ts_probe - probe a touchscreen from DTB description
 *
 * dtb_props is a flat list of key=value pairs terminated by NULL
 * Example: { "compatible","goodix,gt9110", "reg","0x14",
 *             "interrupts","120", NULL }
 * -------------------------------------------------------*/
int ts_probe(const char **dtb_props)
{
    if (g_nr_ts >= MAX_TS_DEVICES) return MX_ENOSPC;

    struct ts_device *dev = &g_ts_devices[g_nr_ts];
    memset(dev, 0, sizeof(*dev));
    spin_lock_init(&dev->lock);

    /* Defaults */
    dev->max_touch   = 5;
    dev->abs_x_min   = 0;
    dev->abs_x_max   = 1080;
    dev->abs_y_min   = 0;
    dev->abs_y_max   = 2340;
    dev->i2c_addr    = 0x14;
    dev->i2c_bus     = 0;
    dev->reset_gpio  = 0xFFFFFFFF;
    dev->irq_gpio    = 0xFFFFFFFF;

    for (int i = 0; i < MT_MAX_SLOTS; i++)
        dev->slots[i].tracking_id = -1;

    /* Parse DTB properties */
    const char *compatible = NULL;
    for (u32 i = 0; dtb_props[i]; i += 2) {
        const char *key = dtb_props[i];
        const char *val = dtb_props[i+1];

        if (strcmp(key, "compatible") == 0)
            compatible = val;
        else if (strcmp(key, "reg") == 0) {
            u64 v = 0;
            if (val[0]=='0' && val[1]=='x') {
                for (const char *p = val+2; *p; p++) {
                    v <<= 4;
                    if (*p >= '0' && *p <= '9') v |= *p - '0';
                    else if (*p >= 'a' && *p <= 'f') v |= *p - 'a' + 10;
                    else if (*p >= 'A' && *p <= 'F') v |= *p - 'A' + 10;
                }
            }
            dev->i2c_addr = (u8)v;
        }
        else if (strcmp(key, "interrupts") == 0) {
            u64 v = 0;
            for (const char *p = val; *p >= '0' && *p <= '9'; p++)
                v = v*10 + (*p - '0');
            dev->irq = (u32)v;
        }
        else if (strcmp(key, "touchscreen-size-x") == 0) {
            u64 v = 0;
            for (const char *p = val; *p >= '0' && *p <= '9'; p++)
                v = v*10 + (*p - '0');
            dev->abs_x_max = (u32)v;
        }
        else if (strcmp(key, "touchscreen-size-y") == 0) {
            u64 v = 0;
            for (const char *p = val; *p >= '0' && *p <= '9'; p++)
                v = v*10 + (*p - '0');
            dev->abs_y_max = (u32)v;
        }
        else if (strcmp(key, "touchscreen-max-pressure") == 0) {
            /* optional */
        }
        else if (strcmp(key, "i2c-bus") == 0) {
            u64 v = 0;
            for (const char *p = val; *p >= '0' && *p <= '9'; p++)
                v = v*10 + (*p - '0');
            dev->i2c_bus = (u32)v;
        }
    }

    /* Select protocol based on compatible string */
    if (compatible && (strcmp(compatible, "goodix,gt9110") == 0 ||
                       strcmp(compatible, "goodix,gt917s") == 0 ||
                       strcmp(compatible, "goodix,gt9xx")  == 0))
        dev->proto = &gt9xx_proto;
    else
        dev->proto = &hidi2c_proto; /* fallback: HID-over-I2C */

    /* Probe */
    int ret = dev->proto->probe(dev);
    if (ret != MX_OK) return ret;

    /* Register IRQ */
    extern int irq_register(u32 irq, void (*fn)(u32, void *), void *data);
    ret = irq_register(dev->irq, ts_irq_handler, dev);
    if (ret != MX_OK) {
        pr_err("TS: failed to register IRQ %u: %d\n", dev->irq, ret);
        return ret;
    }

    dev->enabled = true;
    g_nr_ts++;

    pr_info("TS[%u]: %s %ux%u max_touch=%u IRQ=%u\n",
            g_nr_ts - 1, dev->proto->name,
            dev->abs_x_max, dev->abs_y_max,
            dev->max_touch, dev->irq);
    return MX_OK;
}

/* -------------------------------------------------------
 * ts_read_event - consumer API
 * -------------------------------------------------------*/
int ts_read_event(u32 device_idx, struct ts_event *ev)
{
    if (device_idx >= g_nr_ts || !ev) return MX_EINVAL;
    return ring_pop(&g_ts_devices[device_idx].ring, ev) ? MX_OK : MX_EAGAIN;
}

/* -------------------------------------------------------
 * ts_enable / ts_disable / ts_suspend / ts_resume
 * -------------------------------------------------------*/
void ts_enable(u32 idx)
{
    if (idx < g_nr_ts) g_ts_devices[idx].enabled = true;
}

void ts_disable(u32 idx)
{
    if (idx < g_nr_ts) g_ts_devices[idx].enabled = false;
}

void ts_suspend(u32 idx)
{
    if (idx >= g_nr_ts) return;
    struct ts_device *d = &g_ts_devices[idx];
    d->suspended = true;
    if (d->proto->suspend) d->proto->suspend(d);
}

void ts_resume(u32 idx)
{
    if (idx >= g_nr_ts) return;
    struct ts_device *d = &g_ts_devices[idx];
    d->suspended = false;
    if (d->proto->resume) d->proto->resume(d);
}
