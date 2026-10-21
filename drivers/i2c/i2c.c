/* MONOX Kernel - I2C core (weak fallbacks)
 *
 * drivers/i2c/i2c.h declared i2c_read/i2c_write/i2c_read16/i2c_write16/
 * i2c_init but no file defined them, so the touchscreen driver (and
 * drivers_all.c) could not link.
 *
 * A real controller driver (DesignWare, QUP, Samsung HSI2C, ...) provides
 * strong definitions of these symbols; until one is linked in, every
 * transfer fails cleanly with MX_ENODEV instead of the kernel failing to
 * link or jumping to a NULL pointer.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "i2c.h"

__weak int i2c_init(u32 bus, phys_addr_t mmio_phys, u32 irq, u32 clk_hz)
{
    (void)mmio_phys; (void)irq; (void)clk_hz;
    pr_warn("I2C: no controller driver linked, bus %u unavailable\n", bus);
    return MX_ENODEV;
}

__weak int i2c_read(u32 bus, u8 addr, u8 *buf, u32 len)
{
    (void)bus; (void)addr; (void)buf; (void)len;
    return MX_ENODEV;
}

__weak int i2c_write(u32 bus, u8 addr, const u8 *buf, u32 len)
{
    (void)bus; (void)addr; (void)buf; (void)len;
    return MX_ENODEV;
}

__weak int i2c_read16(u32 bus, u8 addr, u16 reg, u8 *buf, u32 len)
{
    (void)bus; (void)addr; (void)reg; (void)buf; (void)len;
    return MX_ENODEV;
}

__weak int i2c_write16(u32 bus, u8 addr, u16 reg, const u8 *buf, u32 len)
{
    (void)bus; (void)addr; (void)reg; (void)buf; (void)len;
    return MX_ENODEV;
}
