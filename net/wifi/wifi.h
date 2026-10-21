/* MONOX Kernel - WiFi Interface */

#ifndef _MONOX_WIFI_H
#define _MONOX_WIFI_H

#include <kernel/types.h>

typedef enum {
    WIFI_SEC_OPEN  = 0,
    WIFI_SEC_WEP   = 1,
    WIFI_SEC_WPA   = 2,
    WIFI_SEC_WPA2  = 3,
    WIFI_SEC_WPA3  = 4,
    WIFI_SEC_WPA2_ENTERPRISE = 5,
} wifi_security_t;

struct wifi_dev;

struct wifi_ops {
    const char *name;
    int  (*probe)      (struct wifi_dev *dev);
    int  (*scan)       (struct wifi_dev *dev, u32 timeout_ms);
    int  (*connect)    (struct wifi_dev *dev, const char *ssid,
                        const char *psk, wifi_security_t sec);
    int  (*disconnect) (struct wifi_dev *dev);
    int  (*get_rssi)   (struct wifi_dev *dev, s16 *rssi_out);
    int  (*set_channel)(struct wifi_dev *dev, u8 channel);
    int  (*send)       (struct wifi_dev *dev, const u8 *frame, u32 len);
    void (*irq_handler)(struct wifi_dev *dev);
    void (*suspend)    (struct wifi_dev *dev);
    void (*resume)     (struct wifi_dev *dev);
};

struct wifi_status {
    bool  connected;
    s16   rssi;
    u32   frequency;
    u8    mac[6];
    char  ssid[33];
};

void  wifi_init(void);
int   wifi_register_chip(const struct wifi_ops *ops, volatile u8 *mmio,
                          u32 irq, const u8 mac[6]);
int   wifi_connect(u32 dev_idx, const char *ssid, const char *psk,
                   wifi_security_t sec);
int   wifi_disconnect(u32 dev_idx);
bool  wifi_is_connected(u32 dev_idx);
void  wifi_get_status(u32 dev_idx, struct wifi_status *st);

#endif /* _MONOX_WIFI_H */
