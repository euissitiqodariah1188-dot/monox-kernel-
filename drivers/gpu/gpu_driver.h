/* MONOX Kernel - GPU Driver Interface */

#ifndef _MONOX_GPU_DRIVER_H
#define _MONOX_GPU_DRIVER_H

#include <kernel/types.h>

typedef enum {
    GPU_VENDOR_UNKNOWN = 0,
    GPU_VENDOR_MALI    = 1,
    GPU_VENDOR_ADRENO  = 2,
    GPU_VENDOR_POWERVR = 3,
    GPU_VENDOR_VIVANTE = 4,
} gpu_vendor_t;

struct gpu_info {
    gpu_vendor_t vendor;
    u32          gpu_id;
    u32          shader_cores;
    u64          gpu_freq_hz;
    u64          vram_total;
    bool         initialized;
    char         name[64];
};

/* GPU memory flags */
#define GPU_MEM_READABLE  0x01
#define GPU_MEM_WRITABLE  0x02
#define GPU_MEM_EXEC      0x04
#define GPU_MEM_CACHED    0x08

int   gpu_init(phys_addr_t mmio_phys, size_t mmio_size,
               u32 irq, const char *compatible);
u32   gpu_mem_alloc(size_t size, u32 flags);
void  gpu_mem_free(u32 handle);
int   gpu_submit_commands(const u8 *cmds, size_t len);
int   gpu_wait_idle(u32 timeout_ms);
void  gpu_get_info(struct gpu_info *info);

/* Called from IRQ layer */
void  gpu_irq_handler(u32 irq, void *data);

#endif /* _MONOX_GPU_DRIVER_H */
