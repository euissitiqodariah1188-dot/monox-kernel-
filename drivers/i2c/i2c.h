/* MONOX Kernel - I2C Driver Interface */

#ifndef _MONOX_I2C_H
#define _MONOX_I2C_H

#include <kernel/types.h>

int i2c_read  (u32 bus, u8 addr, u8 *buf, u32 len);
int i2c_write (u32 bus, u8 addr, const u8 *buf, u32 len);
int i2c_read16(u32 bus, u8 addr, u16 reg, u8 *buf, u32 len);
int i2c_write16(u32 bus, u8 addr, u16 reg, const u8 *buf, u32 len);
int i2c_init  (u32 bus, phys_addr_t mmio_phys, u32 irq, u32 clk_hz);

#endif /* _MONOX_I2C_H */
