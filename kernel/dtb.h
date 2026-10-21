/* MONOX Kernel - DTB Parser Interface */

#ifndef _MONOX_DTB_H
#define _MONOX_DTB_H

#include <kernel/types.h>
#include "../mm/pmm.h"

struct board_info {
    char        model[128];
    u64         total_ram;

    struct mem_region regions[128];   /* RAM + reserved-memory + /memreserve/ */
    u32         nr_regions;

    bool        psci_hvc;             /* DTB psci/method == "hvc" (QEMU without EL2) */

    phys_addr_t dtb_phys;             /* where the DTB itself lives (reserve it!) */
    size_t      dtb_size;

    phys_addr_t uart_base;

    phys_addr_t gic_dist_base;
    phys_addr_t gic_redist_base;
    u32         gic_redist_stride;

    phys_addr_t gpu_mmio;
    size_t      gpu_mmio_size;
    u32         gpu_irq;
    char        gpu_compatible[64];

    phys_addr_t xhci_mmio;
    size_t      xhci_mmio_size;
    u32         xhci_irq;

    phys_addr_t wifi_mmio;
    u32         wifi_irq;
    u8          wifi_mac[6];
    char        wifi_compatible[64];

    const char *ts_dtb_props[32];

    phys_addr_t kernel_phys_base;
    size_t      kernel_size;
};

void dtb_parse(phys_addr_t dtb_phys, struct board_info *out);

#endif /* _MONOX_DTB_H */
