/* MONOX Kernel - USB Driver Interface */

#ifndef _MONOX_USB_H
#define _MONOX_USB_H

#include <kernel/types.h>

typedef enum {
    USB_SPEED_FULL  = 0,
    USB_SPEED_LOW   = 1,
    USB_SPEED_HIGH  = 2,
    USB_SPEED_SUPER = 3,
    USB_SPEED_SUPER_PLUS = 4,
} usb_speed_t;

int  usb_init(phys_addr_t xhci_phys, size_t xhci_sz, u32 irq);
int  usb_enumerate_device(u32 slot_id, usb_speed_t speed);
int  usb_hid_register_interrupt(u32 dev_id, u8 interface, u8 endpoint,
                                  u32 max_pkt,
                                  void (*cb)(u32, const u8 *, u32, void *),
                                  void *ctx);
int  usb_hid_set_report(u32 dev_id, u8 interface, u8 report_type,
                         u8 report_id, const u8 *data, u32 len);
int  usb_file_transfer_start(u32 dev_id, const char *remote, const char *local,
                              bool upload);
int  usb_run_file(u32 dev_id, const char *path);
void usb_stats(void);

/* Called from IRQ layer */
void usb_irq_handler(u32 irq, void *data);

/* USB class codes */
#define USB_CLASS_HID          0x03
#define USB_CLASS_MASS_STORAGE 0x08

#endif /* _MONOX_USB_H */
