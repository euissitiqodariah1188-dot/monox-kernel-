/* MONOX Kernel - USB Host Controller Driver
 *
 * Supports: USB 3.2 (xHCI), USB 3.0, USB-C (via xHCI)
 * Features:
 *   - xHCI controller support (base addr from DTB)
 *   - Device enumeration
 *   - HID (mouse/keyboard), mass storage, CDC (serial)
 *   - USB-C: file transfer, device control, OTG
 *   - No hardcoded MMIO addresses
 *
 * Bug-fix pass:
 *  - XHCI_USBCMD_HCRST was missing a space (macro with a bogus parameter
 *    list), usb_speed_t / usb_irq_handler were declared twice with
 *    conflicting storage class, satpam_result_t was undeclared.
 *  - The controller was started (Run/Stop=1) with DCBAAP, the command ring
 *    and the event ring all still zero: on the first port event the xHC
 *    DMA-writes to physical address 0. Rings (and scratchpad buffers, which
 *    the spec makes mandatory when the controller asks for them) are now
 *    allocated and programmed BEFORE Run is set, and the event ring is
 *    drained/acknowledged in the IRQ handler.
 *  - The IRQ handler handed an UNINITIALISED stack buffer to the HID
 *    callbacks (random key/mouse events on every interrupt) and could read
 *    past it when max_pkt > 64.
 *  - IRQ-shared lock taken without disabling interrupts => self-deadlock.
 *  - Device table slots were never reused (permanent ENOSPC after 32
 *    enumerations); MMIO mapping and DMA memory leaked on every error path.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "usb.h"
#include "../../mm/vmm.h"
#include "../../mm/pmm.h"
#include "../../arch/arm64/cpu/irq.h"
#include "../../security/satpam/traffic_satpam.h"

/* -------------------------------------------------------
 * xHCI capability register offsets
 * -------------------------------------------------------*/
#define XHCI_CAPLENGTH   0x00   /* capability register length        */
#define XHCI_HCIVERSION  0x02   /* host controller interface version */
#define XHCI_HCSPARAMS1  0x04   /* structural params 1               */
#define XHCI_HCSPARAMS2  0x08   /* structural params 2               */
#define XHCI_HCSPARAMS3  0x0C   /* structural params 3               */
#define XHCI_HCCPARAMS1  0x10   /* capability params 1               */
#define XHCI_DBOFF       0x14   /* doorbell array offset             */
#define XHCI_RTSOFF      0x18   /* runtime register space offset     */

/* xHCI operational register offsets (relative to cap_length) */
#define XHCI_USBCMD      0x00
#define XHCI_USBSTS      0x04
#define XHCI_PAGESIZE    0x08
#define XHCI_DNCTRL      0x14
#define XHCI_CRCR        0x18
#define XHCI_DCBAAP      0x30
#define XHCI_CONFIG      0x38
#define XHCI_PORTSC(n)   (0x400 + (n)*0x10)

#define XHCI_USBCMD_RUN  (1 << 0)
#define XHCI_USBCMD_HCRST (1 << 1)
#define XHCI_USBCMD_INTE  (1 << 2)
#define XHCI_USBSTS_HCH   (1 << 0)
#define XHCI_USBSTS_EINT  (1 << 3)
#define XHCI_USBSTS_CNR   (1 << 11)

/* Runtime registers: interrupter 0 at rt + 0x20 */
#define XHCI_IR0          0x20
#define XHCI_IMAN         0x00
#define XHCI_IMOD         0x04
#define XHCI_ERSTSZ       0x08
#define XHCI_ERSTBA       0x10
#define XHCI_ERDP         0x18
#define XHCI_IMAN_IP      (1 << 0)
#define XHCI_IMAN_IE      (1 << 1)
#define XHCI_ERDP_EHB     (1ULL << 3)

#define XHCI_CMD_RING_TRBS   64
#define XHCI_EVT_RING_TRBS   256
#define TRB_TYPE_LINK        6

struct xhci_trb { u64 param; u32 status; u32 control; };

/* -------------------------------------------------------
 * USB device state
 * -------------------------------------------------------*/
typedef enum {
    USB_STATE_DETACHED  = 0,
    USB_STATE_ATTACHED  = 1,
    USB_STATE_POWERED   = 2,
    USB_STATE_DEFAULT   = 3,
    USB_STATE_ADDRESS   = 4,
    USB_STATE_CONFIGURED= 5,
    USB_STATE_SUSPENDED = 6,
} usb_state_t;

/* usb_speed_t comes from usb.h (the copy that used to be here was a redefinition) */

/* -------------------------------------------------------
 * USB device descriptor (from spec)
 * -------------------------------------------------------*/
struct usb_device_descriptor {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    u8  bMaxPacketSize0;
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice;
    u8  iManufacturer;
    u8  iProduct;
    u8  iSerialNumber;
    u8  bNumConfigurations;
} __packed;

struct usb_config_descriptor {
    u8  bLength;
    u8  bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces;
    u8  bConfigurationValue;
    u8  iConfiguration;
    u8  bmAttributes;
    u8  bMaxPower;
} __packed;

struct usb_interface_descriptor {
    u8 bLength;
    u8 bDescriptorType;
    u8 bInterfaceNumber;
    u8 bAlternateSetting;
    u8 bNumEndpoints;
    u8 bInterfaceClass;
    u8 bInterfaceSubClass;
    u8 bInterfaceProtocol;
    u8 iInterface;
} __packed;

struct usb_endpoint_descriptor {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress;
    u8  bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} __packed;

/* bInterfaceClass */
#define USB_CLASS_HID           0x03
#define USB_CLASS_MASS_STORAGE  0x08
#define USB_CLASS_CDC           0x0A
#define USB_CLASS_HUB           0x09

/* bInterfaceSubClass for HID */
#define USB_HID_SUBCLASS_BOOT   0x01

/* bInterfaceProtocol for HID Boot */
#define USB_HID_PROTO_KEYBOARD  0x01
#define USB_HID_PROTO_MOUSE     0x02

/* -------------------------------------------------------
 * Per-device tracked info
 * -------------------------------------------------------*/
#define MAX_USB_DEVICES  32
#define MAX_USB_ENDPOINTS 8

struct usb_endpoint {
    u8   address;
    u8   type;        /* 0=ctrl 1=iso 2=bulk 3=int */
    u16  max_pkt;
    u8   interval;
    bool in;
    /* Interrupt transfer callback */
    void (*int_cb)(u32 dev_id, const u8 *data, u32 len, void *ctx);
    void *int_ctx;
    u32   int_len;
};

struct usb_device {
    u32                      id;
    usb_state_t              state;
    usb_speed_t              speed;
    u8                       address;
    u8                       slot_id;
    struct usb_device_descriptor desc;
    struct usb_endpoint      eps[MAX_USB_ENDPOINTS];
    u32                      nr_eps;
    bool                     valid;
    char                     product[64];
    char                     manufacturer[64];
};

/* -------------------------------------------------------
 * xHCI host controller
 * -------------------------------------------------------*/
#define MAX_XHCI_CONTROLLERS 4

struct xhci_ctrl {
    volatile u8  *cap_regs;    /* capability registers (VA) */
    volatile u8  *op_regs;     /* operational registers (VA)*/
    volatile u32 *doorbell;    /* doorbell array (VA)       */
    volatile u8  *rt_regs;     /* runtime registers (VA)    */
    u32           irq;
    u32           nr_ports;
    u32           nr_slots;
    bool          valid;

    /* DMA structures (physical addresses; virtual via phys_to_virt_pmm) */
    phys_addr_t   dcbaa_pa;
    phys_addr_t   scratch_arr_pa;   u32 scratch_arr_order;
    phys_addr_t   scratch_pa;       u32 scratch_order;  u32 nr_scratch;
    phys_addr_t   cmd_ring_pa;
    phys_addr_t   evt_ring_pa;
    phys_addr_t   erst_pa;
    u32           evt_deq;          /* consumer index           */
    u32           evt_ccs;          /* consumer cycle state     */
    u64           events_seen;
    u64           va_base;          /* mapping to release       */
    size_t        map_size;
};

static struct xhci_ctrl  g_xhci[MAX_XHCI_CONTROLLERS];
static u32               g_nr_xhci = 0;

static struct usb_device g_usb_devs[MAX_USB_DEVICES];
static u32               g_nr_usb_devs = 0;
static spinlock_t        g_usb_lock = SPIN_LOCK_INIT;
static u32               g_next_dev_id = 1;

/* -------------------------------------------------------
 * xhci MMIO accessors
 * -------------------------------------------------------*/
static __always_inline u32 xhci_cap_read32(struct xhci_ctrl *c, u32 off)
{
    return READ_ONCE(*(volatile u32 *)(c->cap_regs + off));
}

static __always_inline u32 xhci_op_read32(struct xhci_ctrl *c, u32 off)
{
    return READ_ONCE(*(volatile u32 *)(c->op_regs + off));
}

static __always_inline void xhci_op_write32(struct xhci_ctrl *c, u32 off, u32 val)
{
    WRITE_ONCE(*(volatile u32 *)(c->op_regs + off), val);
    dsb(sy);
}

static __always_inline void xhci_op_write64(struct xhci_ctrl *c, u32 off, u64 val)
{
    /* low dword first, as required for 32-bit access to 64-bit registers */
    WRITE_ONCE(*(volatile u32 *)(c->op_regs + off),     (u32)val);
    WRITE_ONCE(*(volatile u32 *)(c->op_regs + off + 4), (u32)(val >> 32));
    dsb(sy);
}

static __always_inline u32 xhci_rt_read32(struct xhci_ctrl *c, u32 off)
{
    return READ_ONCE(*(volatile u32 *)(c->rt_regs + XHCI_IR0 + off));
}

static __always_inline void xhci_rt_write32(struct xhci_ctrl *c, u32 off, u32 val)
{
    WRITE_ONCE(*(volatile u32 *)(c->rt_regs + XHCI_IR0 + off), val);
    dsb(sy);
}

static __always_inline void xhci_rt_write64(struct xhci_ctrl *c, u32 off, u64 val)
{
    WRITE_ONCE(*(volatile u32 *)(c->rt_regs + XHCI_IR0 + off),     (u32)val);
    WRITE_ONCE(*(volatile u32 *)(c->rt_regs + XHCI_IR0 + off + 4), (u32)(val >> 32));
    dsb(sy);
}

/* -------------------------------------------------------
 * xhci_reset - reset xHCI controller
 * -------------------------------------------------------*/
static int xhci_reset(struct xhci_ctrl *c)
{
    /* Stop the controller */
    u32 cmd = xhci_op_read32(c, XHCI_USBCMD);
    cmd &= ~XHCI_USBCMD_RUN;
    xhci_op_write32(c, XHCI_USBCMD, cmd);

    /* Wait for HCH (halted). 'timeout--' used as the loop condition left
     * timeout at UINT_MAX after expiring, so "!timeout" could never detect
     * a timeout; use an explicit result instead. */
    bool ok = false;
    for (u32 t = 100000; t; t--) {
        if (xhci_op_read32(c, XHCI_USBSTS) & XHCI_USBSTS_HCH) { ok = true; break; }
        cpu_relax();
    }
    if (!ok) return MX_ETIMEDOUT;

    /* Reset */
    xhci_op_write32(c, XHCI_USBCMD, XHCI_USBCMD_HCRST);
    ok = false;
    for (u32 t = 100000; t; t--) {
        if (!(xhci_op_read32(c, XHCI_USBCMD) & XHCI_USBCMD_HCRST)) { ok = true; break; }
        cpu_relax();
    }
    if (!ok) return MX_ETIMEDOUT;

    /* registers must not be touched until Controller Not Ready clears */
    for (u32 t = 100000; t; t--) {
        if (!(xhci_op_read32(c, XHCI_USBSTS) & XHCI_USBSTS_CNR)) return MX_OK;
        cpu_relax();
    }
    return MX_ETIMEDOUT;
}

static u32 pages_to_order(u32 pages)
{
    u32 o = 0;
    while ((1U << o) < pages) o++;
    return o;
}

static void xhci_free_dma(struct xhci_ctrl *c)
{
    if (c->dcbaa_pa)       pmm_free_pages(c->dcbaa_pa, 0);
    if (c->scratch_arr_pa) pmm_free_pages(c->scratch_arr_pa, c->scratch_arr_order);
    if (c->scratch_pa)     pmm_free_pages(c->scratch_pa, c->scratch_order);
    if (c->cmd_ring_pa)    pmm_free_pages(c->cmd_ring_pa, 0);
    if (c->evt_ring_pa)    pmm_free_pages(c->evt_ring_pa, 0);
    if (c->erst_pa)        pmm_free_pages(c->erst_pa, 0);
    c->dcbaa_pa = c->scratch_arr_pa = c->scratch_pa = 0;
    c->cmd_ring_pa = c->evt_ring_pa = c->erst_pa = 0;
}

/* Allocate and program DCBAA, command ring, event ring and scratchpads.
 * Everything comes from ZONE_DMA (<4 GiB) so controllers without 64-bit
 * addressing (HCCPARAMS1.AC64 = 0) can reach it. */
static int xhci_setup_rings(struct xhci_ctrl *c)
{
    const gfp_t gfp = GFP_DMA | __GFP_ZERO;

    c->dcbaa_pa    = pmm_alloc_pages(0, gfp);
    c->cmd_ring_pa = pmm_alloc_pages(0, gfp);
    c->evt_ring_pa = pmm_alloc_pages(0, gfp);
    c->erst_pa     = pmm_alloc_pages(0, gfp);
    if (!c->dcbaa_pa || !c->cmd_ring_pa || !c->evt_ring_pa || !c->erst_pa) {
        xhci_free_dma(c);
        return MX_ENOMEM;
    }

    /* Scratchpad buffers: when HCSPARAMS2 asks for them, DCBAA[0] MUST point
     * at an array of their addresses or the controller faults. */
    u32 hcs2 = xhci_cap_read32(c, XHCI_HCSPARAMS2);
    u32 nscr = ((hcs2 >> 21) & 0x1F) | (((hcs2 >> 27) & 0x1F) << 5);
    c->nr_scratch = nscr;
    if (nscr) {
        c->scratch_order     = pages_to_order(nscr);
        c->scratch_arr_order = 0;               /* up to 1023 * 8 bytes < 4 KiB */
        c->scratch_arr_pa    = pmm_alloc_pages(0, gfp);
        c->scratch_pa        = pmm_alloc_pages(c->scratch_order, gfp);
        if (!c->scratch_arr_pa || !c->scratch_pa) {
            xhci_free_dma(c);
            return MX_ENOMEM;
        }
        u64 *arr = (u64 *)phys_to_virt_pmm(c->scratch_arr_pa);
        for (u32 i = 0; i < nscr; i++)
            arr[i] = c->scratch_pa + (u64)i * 4096;
        cache_flush_range(arr, nscr * sizeof(u64));
        ((u64 *)phys_to_virt_pmm(c->dcbaa_pa))[0] = c->scratch_arr_pa;
    }
    cache_flush_range(phys_to_virt_pmm(c->dcbaa_pa), 4096);

    /* Command ring: 64 TRBs, the last one is a Link TRB back to the start */
    struct xhci_trb *cr = (struct xhci_trb *)phys_to_virt_pmm(c->cmd_ring_pa);
    cr[XHCI_CMD_RING_TRBS - 1].param   = c->cmd_ring_pa;
    cr[XHCI_CMD_RING_TRBS - 1].control = (TRB_TYPE_LINK << 10) | (1 << 1); /* TC */
    cache_flush_range(cr, XHCI_CMD_RING_TRBS * sizeof(struct xhci_trb));

    /* Event ring: one segment of 256 TRBs described by a 1-entry ERST */
    u64 *erst = (u64 *)phys_to_virt_pmm(c->erst_pa);
    erst[0] = c->evt_ring_pa;
    erst[1] = XHCI_EVT_RING_TRBS;               /* size in TRBs (low 32), rsvd=0 */
    cache_flush_range(erst, 16);
    cache_flush_range(phys_to_virt_pmm(c->evt_ring_pa), 4096);
    c->evt_deq = 0;
    c->evt_ccs = 1;

    /* Program registers (spec order: DCBAAP, CRCR, then interrupter 0) */
    xhci_op_write64(c, XHCI_DCBAAP, c->dcbaa_pa);
    xhci_op_write64(c, XHCI_CRCR,   c->cmd_ring_pa | 1 /* RCS */);

    xhci_rt_write32(c, XHCI_ERSTSZ, 1);
    xhci_rt_write64(c, XHCI_ERDP,   c->evt_ring_pa);
    xhci_rt_write64(c, XHCI_ERSTBA, c->erst_pa);     /* last: arms the ring */
    xhci_rt_write32(c, XHCI_IMOD,   4000);           /* ~1 ms moderation */
    xhci_rt_write32(c, XHCI_IMAN,   XHCI_IMAN_IP | XHCI_IMAN_IE);
    return MX_OK;
}

/* -------------------------------------------------------
 * usb_init - initialise USB host from DTB-provided addresses
 * -------------------------------------------------------*/
int usb_init(phys_addr_t xhci_phys, size_t xhci_sz, u32 irq)
{
    if (g_nr_xhci >= MAX_XHCI_CONTROLLERS) return MX_ENOSPC;

    struct xhci_ctrl *c = &g_xhci[g_nr_xhci];
    memset(c, 0, sizeof(*c));

    /* Map xHCI MMIO */
    if (xhci_sz < 0x1000) xhci_sz = 0x1000;       /* DTB without a size */
    u64 va = vmm_map_phys(xhci_phys, xhci_sz, 0); /* device memory */
    if (!va) return MX_ENOMEM;
    c->va_base  = va;
    c->map_size = xhci_sz;

    c->cap_regs = (volatile u8 *)va;
    u8 cap_len  = *(volatile u8 *)va; /* CAPLENGTH byte */
    c->op_regs  = c->cap_regs + cap_len;

    u32 dboff  = xhci_cap_read32(c, XHCI_DBOFF)  & ~0x3U;
    u32 rtsoff = xhci_cap_read32(c, XHCI_RTSOFF) & ~0x1FU;
    /* registers beyond the mapping would fault: reject nonsense offsets */
    if (cap_len < 0x20 || dboff + 4 > xhci_sz || rtsoff + XHCI_IR0 + 0x20 > xhci_sz) {
        pr_err("USB: xHCI @ 0x%llx has bogus register offsets (not an xHCI?)\n", xhci_phys);
        vmm_unmap(va, xhci_sz);
        return MX_ENODEV;
    }
    c->doorbell = (volatile u32 *)(va + dboff);
    c->rt_regs  = c->cap_regs + rtsoff;
    c->irq      = irq;

    /* Parse HCSPARAMS1 */
    u32 hcs1    = xhci_cap_read32(c, XHCI_HCSPARAMS1);
    c->nr_slots = hcs1 & 0xFF;
    c->nr_ports = (hcs1 >> 24) & 0xFF;

    pr_info("USB: xHCI @ 0x%llx IRQ=%u ports=%u slots=%u\n",
            xhci_phys, irq, c->nr_ports, c->nr_slots);

    /* Reset controller */
    int ret = xhci_reset(c);
    if (ret) {
        pr_err("USB: xHCI reset failed\n");
        vmm_unmap(va, xhci_sz);
        return ret;
    }

    /* Configure max slots */
    xhci_op_write32(c, XHCI_CONFIG, c->nr_slots);

    /* DCBAA / rings / scratchpads must exist BEFORE Run is set */
    ret = xhci_setup_rings(c);
    if (ret) {
        pr_err("USB: xHCI ring setup failed (%d)\n", ret);
        vmm_unmap(va, xhci_sz);
        return ret;
    }

    /* Register IRQ (0 = DTB had none: run polled, nothing to register) */
    extern int irq_register(u32, void (*)(u32, void *), void *);
    if (irq) {
        ret = irq_register(irq, usb_irq_handler, c);
        if (ret) {
            xhci_free_dma(c);
            vmm_unmap(va, xhci_sz);
            return ret;
        }
    }

    c->valid = true;

    /* Start controller (interrupts enabled) */
    u32 cmd = xhci_op_read32(c, XHCI_USBCMD);
    cmd |= XHCI_USBCMD_RUN | XHCI_USBCMD_INTE;
    xhci_op_write32(c, XHCI_USBCMD, cmd);

    g_nr_xhci++;
    return MX_OK;
}

/* -------------------------------------------------------
 * usb_irq_handler - called when xHCI fires IRQ
 * -------------------------------------------------------*/
void usb_irq_handler(u32 irq, void *data)
{
    (void)irq;
    struct xhci_ctrl *c = (struct xhci_ctrl *)data;
    if (!c || !c->valid) return;

    /* Acknowledge: USBSTS.EINT (W1C) and IMAN.IP (W1C) */
    u32 sts = xhci_op_read32(c, XHCI_USBSTS);
    xhci_op_write32(c, XHCI_USBSTS, sts & XHCI_USBSTS_EINT);
    u32 iman = xhci_rt_read32(c, XHCI_IMAN);
    xhci_rt_write32(c, XHCI_IMAN, iman | XHCI_IMAN_IP);

    /* Drain the event ring so the controller never stalls on a full ring.
     * Decoding the events (port status change, transfer complete) needs the
     * transfer-ring machinery, which this driver does not have yet, so the
     * HID callbacks are NOT invoked: the old code called them with an
     * uninitialised buffer and generated random input events. */
    struct xhci_trb *ring = (struct xhci_trb *)phys_to_virt_pmm(c->evt_ring_pa);
    for (u32 n = 0; n < XHCI_EVT_RING_TRBS; n++) {
        struct xhci_trb *t = &ring[c->evt_deq];
        cache_flush_range(t, sizeof(*t));       /* see the controller's write */
        if ((t->control & 1) != c->evt_ccs)
            break;                              /* no more events */
        c->events_seen++;
        if (++c->evt_deq == XHCI_EVT_RING_TRBS) {
            c->evt_deq = 0;
            c->evt_ccs ^= 1;
        }
    }
    /* tell the controller how far we got (and clear Event Handler Busy) */
    xhci_rt_write64(c, XHCI_ERDP,
                    (c->evt_ring_pa + (u64)c->evt_deq * sizeof(struct xhci_trb))
                    | XHCI_ERDP_EHB);
}

/* -------------------------------------------------------
 * usb_hid_register_interrupt
 * -------------------------------------------------------*/
int usb_hid_register_interrupt(u32 dev_id, u8 interface, u8 endpoint,
                                 u32 max_pkt,
                                 void (*cb)(u32, const u8 *, u32, void *),
                                 void *ctx)
{
    u64 irqf;
    spin_lock_irqsave(&g_usb_lock, irqf);
    for (u32 i = 0; i < g_nr_usb_devs; i++) {
        if (g_usb_devs[i].id != dev_id || !g_usb_devs[i].valid) continue;
        struct usb_device *d = &g_usb_devs[i];
        for (u32 e = 0; e < d->nr_eps; e++) {
            if (d->eps[e].address == endpoint) {
                d->eps[e].int_cb  = cb;
                d->eps[e].int_ctx = ctx;
                d->eps[e].int_len = max_pkt;
                spin_unlock_irqrestore(&g_usb_lock, irqf);
                return MX_OK;
            }
        }
        /* Endpoint not registered yet, add it */
        if (d->nr_eps < MAX_USB_ENDPOINTS) {
            struct usb_endpoint *ep = &d->eps[d->nr_eps++];
            ep->address  = endpoint;
            ep->type     = 3; /* interrupt */
            ep->in       = true;
            ep->max_pkt  = (u16)max_pkt;
            ep->int_cb   = cb;
            ep->int_ctx  = ctx;
            ep->int_len  = max_pkt;
            spin_unlock_irqrestore(&g_usb_lock, irqf);
            return MX_OK;
        }
    }
    spin_unlock_irqrestore(&g_usb_lock, irqf);
    return MX_ENODEV;
}

/* -------------------------------------------------------
 * usb_hid_set_report - send HID output report (e.g. LED state)
 * -------------------------------------------------------*/
int usb_hid_set_report(u32 dev_id, u8 interface, u8 report_type,
                        u8 report_id, const u8 *data, u32 len)
{
    /* In a full implementation: issue a SETUP + DATA OUT transfer via xHCI */
    /* For now: log and return OK (LEDs work when real xHCI TRB is wired up) */
    (void)dev_id; (void)interface; (void)report_type;
    (void)report_id; (void)data; (void)len;
    return MX_OK;
}

/* -------------------------------------------------------
 * usb_enumerate_device - parse descriptors and register device
 * -------------------------------------------------------*/
int usb_enumerate_device(u32 slot_id, usb_speed_t speed)
{
    u64 irqf;
    spin_lock_irqsave(&g_usb_lock, irqf);

    /* reuse a free slot: the old code always used g_nr_usb_devs, so after 32
     * enumerations (even with devices unplugged) it returned ENOSPC forever */
    struct usb_device *d = NULL;
    for (u32 i = 0; i < MAX_USB_DEVICES; i++) {
        if (!g_usb_devs[i].valid) {
            d = &g_usb_devs[i];
            if (i >= g_nr_usb_devs) g_nr_usb_devs = i + 1;   /* high-water mark */
            break;
        }
    }
    if (!d) {
        spin_unlock_irqrestore(&g_usb_lock, irqf);
        return MX_ENOSPC;
    }

    memset(d, 0, sizeof(*d));
    d->id      = g_next_dev_id++;
    d->state   = USB_STATE_ADDRESS;
    d->speed   = speed;
    d->slot_id = (u8)slot_id;
    d->valid   = true;
    u32 dev_id = d->id;
    spin_unlock_irqrestore(&g_usb_lock, irqf);

    pr_info("USB: device %u enumerated (slot=%u speed=%u)\n",
            dev_id, slot_id, speed);

    /* Real code would issue GET_DESCRIPTOR, parse and probe class drivers */
    /* Class driver probe would call mouse_probe / kbd_probe etc. */

    return MX_OK;
}

/* -------------------------------------------------------
 * usb_file_transfer_start - USB-C file transfer (requirement)
 * -------------------------------------------------------*/
int usb_file_transfer_start(u32 dev_id, const char *remote_path,
                              const char *local_path, bool upload)
{
    /* Uses USB Mass Storage (BOT) or MTP protocol over USB-C */
    pr_info("USB: file transfer %s %s↔%s dev=%u\n",
            upload ? "upload" : "download", local_path, remote_path, dev_id);
    /* Full implementation: create async transfer task via scheduler */
    return MX_OK;
}

/* -------------------------------------------------------
 * usb_run_file - run a file received over USB-C (requirement)
 * -------------------------------------------------------*/
int usb_run_file(u32 dev_id, const char *path)
{
    if (!path) return MX_EINVAL;
    /* Security check via satpam before executing */
    extern pid_t current_pid(void);
    if (satpam_check_route("usb_run", "exec.userspace", current_pid()) != 0) {
        pr_warn("USB: run file rejected by satpam: %s\n", path);
        return MX_EPERM;
    }

    pr_info("USB: run file '%s' from device %u\n", path, dev_id);
    /* In a full OS: exec() the file after copying to /tmp */
    return MX_OK;
}

/* -------------------------------------------------------
 * usb_stats
 * -------------------------------------------------------*/
void usb_stats(void)
{
    pr_info("USB: %u controllers, %u devices\n", g_nr_xhci, g_nr_usb_devs);
}
