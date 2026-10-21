/* MONOX Kernel - WiFi / Network Manager
 *
 * Supports:
 *   - Any WiFi chip via cfg80211-compatible driver interface
 *   - Auto-connect from saved networks (read from /etc/monox/wifi.Config)
 *   - WPA2/WPA3 via software key management
 *   - Ethernet (via net/ethernet/eth.c)
 *   - No hardcoded SSIDs, passwords, or chip addresses
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "wifi.h"
#include "../../fs/vfs/vfs.h"
#include "../../mm/vmm.h"

/* wifi_security_t and struct wifi_ops are declared in wifi.h (the copies that
 * used to be here were redefinition errors) */

/* -------------------------------------------------------
 * Saved network config (from /etc/monox/wifi.Config)
 * -------------------------------------------------------*/
#define MAX_SSID_LEN    33
#define MAX_PSK_LEN     65
#define MAX_SAVED_NETS  32

struct saved_network {
    char           ssid[MAX_SSID_LEN];
    char           psk[MAX_PSK_LEN];
    wifi_security_t sec;
    u8             bssid[6];    /* zero = any BSSID */
    s16            priority;    /* higher = preferred */
    bool           valid;
    bool           auto_connect;
};

/* -------------------------------------------------------
 * Scan result
 * -------------------------------------------------------*/
struct wifi_bss {
    u8              bssid[6];
    char            ssid[MAX_SSID_LEN];
    wifi_security_t sec;
    s16             rssi;         /* dBm */
    u32             frequency;    /* MHz */
    u8              channel;
    bool            valid;
};

#define MAX_SCAN_RESULTS 64

/* -------------------------------------------------------
 * WiFi device
 * -------------------------------------------------------*/
typedef enum {
    WIFI_STATE_OFF         = 0,
    WIFI_STATE_IDLE        = 1,
    WIFI_STATE_SCANNING    = 2,
    WIFI_STATE_CONNECTING  = 3,
    WIFI_STATE_CONNECTED   = 4,
    WIFI_STATE_DISCONNECTING=5,
} wifi_state_t;

struct wifi_dev {
    const struct wifi_ops *ops;
    volatile u8  *mmio;         /* chip MMIO VA */
    u32           irq;
    u8            mac[6];
    wifi_state_t  state;
    char          connected_ssid[MAX_SSID_LEN];
    s16           rssi;
    u32           frequency;
    bool          valid;
    spinlock_t    lock;

    struct wifi_bss scan_results[MAX_SCAN_RESULTS];
    u32             nr_scan_results;

    /* Receive callback (set by network stack) */
    void (*rx_cb)(struct wifi_dev *dev, const u8 *frame, u32 len);
};

/* -------------------------------------------------------
 * Saved networks database
 * -------------------------------------------------------*/
static struct saved_network g_saved_nets[MAX_SAVED_NETS];
static u32                  g_nr_saved_nets = 0;
static spinlock_t           g_net_db_lock = SPIN_LOCK_INIT;

/* -------------------------------------------------------
 * Active WiFi devices
 * -------------------------------------------------------*/
#define MAX_WIFI_DEVS 4
static struct wifi_dev g_wifi_devs[MAX_WIFI_DEVS];
static u32             g_nr_wifi_devs = 0;
static spinlock_t      g_wifi_lock = SPIN_LOCK_INIT;

/* -------------------------------------------------------
 * Config parser - reads /etc/monox/wifi.Config
 * Format:
 *   [network]
 *   ssid=MyNetwork
 *   psk=mypassword
 *   security=wpa2
 *   priority=10
 *   auto_connect=yes
 * -------------------------------------------------------*/
static void wifi_load_saved_networks(void)
{
    struct vfs_file *f = vfs_open("/etc/monox/wifi.Config", 0);
    if (!f) {
        pr_info("WIFI: no saved networks (/etc/monox/wifi.Config not found)\n");
        return;
    }

    u8 buf[4096];
    s64 n = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_close(f);
    if (n <= 0) { memset(buf, 0, sizeof(buf)); return; }
    buf[n] = '\0';

    struct saved_network cur;
    memset(&cur, 0, sizeof(cur));
    bool in_network = false;

    char *line = (char *)buf;
    while (*line) {
        while (*line == '\n' || *line == '\r' || *line == ' ') line++;
        if (!*line) break;

        if (strncmp(line, "[network]", 9) == 0) {
            if (in_network && strlen(cur.ssid) > 0) {
                spin_lock(&g_net_db_lock);
                if (g_nr_saved_nets < MAX_SAVED_NETS) {
                    cur.valid = true;
                    g_saved_nets[g_nr_saved_nets++] = cur;
                }
                spin_unlock(&g_net_db_lock);
            }
            memset(&cur, 0, sizeof(cur));
            cur.priority     = 0;
            cur.auto_connect = true;
            cur.sec          = WIFI_SEC_WPA2;
            in_network       = true;
            while (*line && *line != '\n') line++;
            continue;
        }

        char *eq = line;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') { while (*line && *line != '\n') line++; continue; }

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        char *end = val;
        while (*end && *end != '\n') end++;
        if (*end) { *end = '\0'; line = end + 1; }
        else      { line = end; }     /* at the terminator: stop (end+1 read uninitialised stack) */

        /* Trim */
        for (char *p = key+strlen(key)-1; p >= key && (*p==' '||*p=='\t'); p--) *p='\0';
        for (char *p = val+strlen(val)-1; p >= val && (*p==' '||*p=='\t'); p--) *p='\0';

        if (strcmp(key, "ssid") == 0)
            strncpy(cur.ssid, val, MAX_SSID_LEN - 1);
        else if (strcmp(key, "psk") == 0)
            strncpy(cur.psk, val, MAX_PSK_LEN - 1);
        else if (strcmp(key, "security") == 0) {
            if      (strcmp(val, "open")  == 0) cur.sec = WIFI_SEC_OPEN;
            else if (strcmp(val, "wep")   == 0) cur.sec = WIFI_SEC_WEP;
            else if (strcmp(val, "wpa")   == 0) cur.sec = WIFI_SEC_WPA;
            else if (strcmp(val, "wpa2")  == 0) cur.sec = WIFI_SEC_WPA2;
            else if (strcmp(val, "wpa3")  == 0) cur.sec = WIFI_SEC_WPA3;
        }
        else if (strcmp(key, "priority") == 0) {
            s16 p = 0;
            for (const char *c = val; *c >= '0' && *c <= '9'; c++)
                p = p * 10 + (*c - '0');
            cur.priority = p;
        }
        else if (strcmp(key, "auto_connect") == 0)
            cur.auto_connect = (strcmp(val, "yes") == 0 ||
                                strcmp(val, "true") == 0 ||
                                strcmp(val, "1")    == 0);
    }

    if (in_network && strlen(cur.ssid) > 0) {
        spin_lock(&g_net_db_lock);
        if (g_nr_saved_nets < MAX_SAVED_NETS) {
            cur.valid = true;
            g_saved_nets[g_nr_saved_nets++] = cur;
        }
        spin_unlock(&g_net_db_lock);
    }

    /* the buffer and 'cur' held pre-shared keys: do not leave them on the stack */
    memset(buf, 0, sizeof(buf));
    memset(&cur, 0, sizeof(cur));

    pr_info("WIFI: loaded %u saved networks\n", g_nr_saved_nets);
}

/* -------------------------------------------------------
 * Auto-connect: scan then pick best saved network
 * -------------------------------------------------------*/
static void wifi_auto_connect_task(void *arg)
{
    struct wifi_dev *dev = (struct wifi_dev *)arg;

    while (1) {
        if (dev->state == WIFI_STATE_CONNECTED) {
            /* Check RSSI; reconnect if too weak */
            s16 rssi;
            rssi = dev->rssi;                 /* keep last value if the chip cannot report */
            if (dev->ops->get_rssi)
                dev->ops->get_rssi(dev, &rssi);
            dev->rssi = rssi;
            if (rssi < -85) {
                pr_info("WIFI: RSSI %d dBm too weak, reconnecting\n", rssi);
                if (dev->ops->disconnect) dev->ops->disconnect(dev);
                dev->state = WIFI_STATE_IDLE;
            } else {
                extern void task_sleep(u64 ns);
                task_sleep(5000000000ULL); /* check every 5 sec */
                continue;
            }
        }

        /* Scan */
        dev->state = WIFI_STATE_SCANNING;
        if (!dev->ops->scan || dev->ops->scan(dev, 3000) != MX_OK) {
            dev->state = WIFI_STATE_IDLE;
            extern void task_sleep(u64 ns);
            task_sleep(10000000000ULL);
            continue;
        }
        dev->state = WIFI_STATE_IDLE;

        /* Find best matching saved network */
        const struct saved_network *best = NULL;
        s16 best_rssi = -200;

        spin_lock(&g_net_db_lock);
        for (u32 n = 0; n < g_nr_saved_nets; n++) {
            const struct saved_network *sn = &g_saved_nets[n];
            if (!sn->valid || !sn->auto_connect) continue;

            for (u32 b = 0; b < dev->nr_scan_results; b++) {
                const struct wifi_bss *bss = &dev->scan_results[b];
                if (!bss->valid) continue;
                if (strcmp(bss->ssid, sn->ssid) != 0) continue;

                s16 effective_rssi = bss->rssi + sn->priority;
                if (effective_rssi > best_rssi) {
                    best_rssi = effective_rssi;
                    best      = sn;
                }
            }
        }
        spin_unlock(&g_net_db_lock);

        if (best) {
            dev->state = WIFI_STATE_CONNECTING;
            int ret = dev->ops->connect ?
                      dev->ops->connect(dev, best->ssid, best->psk, best->sec) : MX_ENOSYS;
            if (ret == MX_OK) {
                dev->state = WIFI_STATE_CONNECTED;
                strncpy(dev->connected_ssid, best->ssid, MAX_SSID_LEN - 1);
                pr_info("WIFI: connected to '%s' (RSSI=%d dBm)\n",
                        best->ssid, best_rssi);
            } else {
                dev->state = WIFI_STATE_IDLE;
                pr_warn("WIFI: connect to '%s' failed: %d\n", best->ssid, ret);
            }
        } else {
            pr_info("WIFI: no matching saved network found in scan\n");
        }

        extern void task_sleep(u64 ns);
        task_sleep(30000000000ULL); /* retry in 30 sec */
    }
}

/* -------------------------------------------------------
 * wifi_register_chip - called by chip driver (generic interface)
 * -------------------------------------------------------*/
int wifi_register_chip(const struct wifi_ops *ops, volatile u8 *mmio,
                        u32 irq, const u8 mac[6])
{
    if (!ops || !ops->name) return MX_EINVAL;

    spin_lock(&g_wifi_lock);
    if (g_nr_wifi_devs >= MAX_WIFI_DEVS) {
        spin_unlock(&g_wifi_lock);
        return MX_ENOSPC;
    }

    struct wifi_dev *dev = &g_wifi_devs[g_nr_wifi_devs];
    memset(dev, 0, sizeof(*dev));
    dev->ops  = ops;
    dev->mmio = mmio;
    dev->irq  = irq;
    if (mac) memcpy(dev->mac, mac, 6);
    dev->state = WIFI_STATE_IDLE;
    dev->valid = true;
    spin_lock_init(&dev->lock);
    g_nr_wifi_devs++;
    spin_unlock(&g_wifi_lock);

    /* print the stored copy: 'mac' may be NULL (it is optional above) */
    pr_info("WIFI: chip '%s' registered MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            ops->name,
            dev->mac[0],dev->mac[1],dev->mac[2],dev->mac[3],dev->mac[4],dev->mac[5]);

    /* Start auto-connect task */
    extern struct task *task_create(const char *, void (*)(void *), void *,
                                     u32, size_t);
    task_create("wifi_auto", wifi_auto_connect_task, dev, 10, 32768);

    return MX_OK;
}

/* -------------------------------------------------------
 * wifi_init - called at boot
 * -------------------------------------------------------*/
void wifi_init(void)
{
    memset(g_saved_nets,  0, sizeof(g_saved_nets));
    memset(g_wifi_devs,   0, sizeof(g_wifi_devs));
    spin_lock_init(&g_net_db_lock);
    spin_lock_init(&g_wifi_lock);
    wifi_load_saved_networks();
    pr_info("WIFI: manager ready\n");
}

/* -------------------------------------------------------
 * Public API
 * -------------------------------------------------------*/
int wifi_connect(u32 dev_idx, const char *ssid, const char *psk,
                  wifi_security_t sec)
{
    if (dev_idx >= g_nr_wifi_devs) return MX_EINVAL;
    struct wifi_dev *d = &g_wifi_devs[dev_idx];
    if (!d->valid || !d->ops->connect) return MX_ENODEV;
    return d->ops->connect(d, ssid, psk, sec);
}

int wifi_disconnect(u32 dev_idx)
{
    if (dev_idx >= g_nr_wifi_devs) return MX_EINVAL;
    struct wifi_dev *d = &g_wifi_devs[dev_idx];
    if (!d->valid || !d->ops->disconnect) return MX_ENODEV;
    return d->ops->disconnect(d);
}

bool wifi_is_connected(u32 dev_idx)
{
    if (dev_idx >= g_nr_wifi_devs) return false;
    return g_wifi_devs[dev_idx].state == WIFI_STATE_CONNECTED;
}

void wifi_get_status(u32 dev_idx, struct wifi_status *st)
{
    if (dev_idx >= g_nr_wifi_devs || !st) return;
    const struct wifi_dev *d = &g_wifi_devs[dev_idx];
    st->connected = (d->state == WIFI_STATE_CONNECTED);
    st->rssi      = d->rssi;
    st->frequency = d->frequency;
    memcpy(st->mac, d->mac, 6);
    strncpy(st->ssid, d->connected_ssid, MAX_SSID_LEN - 1);
    st->ssid[MAX_SSID_LEN - 1] = '\0';   /* strncpy does not terminate a full-length SSID */
}
