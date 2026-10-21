/* MONOX Kernel - Physical Memory Manager Interface */

#ifndef _MONOX_PMM_H
#define _MONOX_PMM_H

#include <kernel/types.h>

/* GFP flags */
#define GFP_KERNEL   0x00u   /* normal kernel allocation   */
#define GFP_DMA      0x01u   /* must be in DMA zone (<4GiB)*/
#define GFP_GPU      0x02u   /* prefer GPU zone (4-8 GiB)  */
#define __GFP_ZERO   0x08u   /* zero pages on alloc        */
#define GFP_ZERO     (GFP_KERNEL | __GFP_ZERO)

/* Memory region types (from DTB) */
typedef enum {
    MEM_TYPE_RAM     = 0,
    MEM_TYPE_RESERVED= 1,
    MEM_TYPE_MMIO    = 2,
    MEM_TYPE_ACPI    = 3,
} mem_type_t;

struct mem_region {
    phys_addr_t  base;
    size_t       size;
    mem_type_t   type;
};

/* Largest buddy block is 2^(PMM_MAX_ORDER-1) pages. It used to be 4 MiB,
 * which made every framebuffer larger than 4 MiB (any phone panel) fail. */
#define PMM_MAX_ORDER 14      /* orders 0..13 => up to 32 MiB contiguous */

/* Init */
void        pmm_init(const struct mem_region *regions, u32 nr,
                     u64 total_ram_bytes);
/* May be called BEFORE pmm_init() (preferred: nothing in the range is ever
 * touched) or after it (the range is carved out of the free lists). */
void        pmm_reserve_range(phys_addr_t base, size_t size);
void        pmm_stats(void);

/* Allocation */
phys_addr_t pmm_alloc_pages(u32 order, gfp_t flags);
void        pmm_free_pages(phys_addr_t pa, u32 order);

/* Helpers */
phys_addr_t pmm_gpu_zone_base(void);
size_t      pmm_gpu_zone_size(void);
phys_addr_t virt_to_phys_pmm(const void *va);
void       *phys_to_virt_pmm(phys_addr_t pa);

static inline phys_addr_t pmm_alloc_page(gfp_t flags)
{
    return pmm_alloc_pages(0, flags);
}

static inline void pmm_free_page(phys_addr_t pa)
{
    pmm_free_pages(pa, 0);
}

#endif /* _MONOX_PMM_H */
