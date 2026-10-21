/* MONOX Kernel - All Drivers Interface */

#ifndef _MONOX_DRIVERS_ALL_H
#define _MONOX_DRIVERS_ALL_H

#include <kernel/types.h>

/* Master init */
void drivers_init_all(void);

/* 1. CPU */
int  cpudrive_init(void);

/* 4. MMC/SD */
int  mmc_init(phys_addr_t mmio, u32 irq, u32 max_clk_hz, u32 bus_width);

/* 5. NVMe */
int  nvme_init(phys_addr_t bar0, u32 irq);

/* 7. Battery */
int  battery_init(u32 i2c_bus, u8 i2c_addr);
void battery_update(void);
u8   battery_soc(void);
s32  battery_voltage(void);
bool battery_charging(void);

/* 9. Display */
int  display_init(phys_addr_t dsi_mmio, u32 irq, u32 w, u32 h, u32 hz, u8 bpp);
void display_enable(bool on);

/* 10. Backlight */
int  backlight_init(phys_addr_t pwm_mmio, u32 channel, u32 period_ns);
void backlight_set(u8 brightness_pct);

/* 11. Thermal */
int  thermal_init(phys_addr_t mmio, s32 crit_mc, s32 hot_mc);
void thermal_update(void);
s32  thermal_get_temp_mc(void);

/* 12. cpufreq */
int  cpufreq_init(phys_addr_t pll_mmio, const u64 *freqs, u32 nr_freqs);
void cpufreq_set(u32 idx);

/* 13. DMA */
int  dma_init(phys_addr_t mmio, u32 nr_channels, u32 irq);
int  dma_memcpy(phys_addr_t dst, phys_addr_t src, size_t len, u32 ch);

/* 24. Vibrator */
int  vibrator_init(phys_addr_t pwm_mmio, u32 channel);
void vibrator_pulse(u32 duration_ms, u8 strength_pct);

/* 25. LED */
int  led_register(phys_addr_t gpio_mmio, u32 gpio_pin);
void led_set(u32 idx, bool on);

/* 26. RTC */
int  rtc_init(phys_addr_t mmio);
u64  rtc_get_unix_time(void);
void rtc_set_unix_time(u64 ts);

/* 27. Watchdog */
int  watchdog_init(phys_addr_t mmio, u32 timeout_ms);
void watchdog_kick(void);

/* 28. HW Crypto */
int  hwcrypto_init(phys_addr_t mmio);
int  hwcrypto_aes_encrypt(const u8 *key, u32 bits,
                            const u8 *in, u8 *out, u32 len);

/* 29. TRNG */
int  trng_init(phys_addr_t mmio);
u64  trng_read(void);

/* 37. Bluetooth */
int  bt_init(u32 uart_bus, u32 irq);
int  bt_power(bool on);

/* 48. PMIC */
int  pmic_init(u32 i2c_bus, u8 i2c_addr);
int  pmic_set_voltage(u8 rail, u32 mv);

/* 49. USB-PD */
int  usbpd_init(u32 i2c_bus, u8 i2c_addr);
u32  usbpd_get_max_power(void);

/* 50. Haptics */
int  haptics_init(u32 i2c_bus, u8 i2c_addr);
void haptics_play(u8 effect_id);

#endif /* _MONOX_DRIVERS_ALL_H */
