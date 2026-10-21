/* MONOX Kernel - MMU Interface */

#ifndef _MONOX_MMU_H
#define _MONOX_MMU_H

#include <kernel/types.h>

/* Linear map:  VA = KERNEL_VA_BASE + PA   (RAM and MMIO)
 * Kernel image: linked at KERNEL_IMAGE_VMA (must match KERNEL_VMA in
 * monox.ld) and mapped to wherever the loader put it, so the same binary
 * boots at 0x40000000 (bare metal), 0x40200000 (QEMU virt: RAM base + 2 MiB)
 * or 0x80080000 (phones). The VA window is disjoint from the linear map and
 * from vmm's kernel VA allocator. (The previous scheme linked the image
 * *inside* the linear map, which only worked when it happened to be loaded at
 * exactly the address it was linked for.)                                  */
#define KERNEL_VA_BASE   0xFFFF000000000000ULL
#define KERNEL_IMAGE_VMA 0xFFFFFFC000000000ULL

/* Mapping attribute sets for mmu_map_range()/mmu_map_linear() callers that
 * do not want to spell PTE bits */
#define MMU_ATTR_RAM_RW   1   /* normal WB, RW, NX      */
#define MMU_ATTR_RAM_RWX  2   /* normal WB, RWX         */
#define MMU_ATTR_DEVICE   3   /* Device-nGnRnE, RW, NX  */

void  mmu_early_init(phys_addr_t phys_base, u64 virt_base, size_t kernel_sz);
int   mmu_map_linear(phys_addr_t pa, size_t size, int attr);
void  mmu_enable(void);
int   mmu_map_range(u64 va, u64 pa, size_t size, u64 flags);
int   mmu_unmap_range(u64 va, size_t size);
int   mmu_map_device(u64 va, u64 pa, size_t size);
void  mmu_protect_kernel(u64 text_va,   size_t text_sz,
                          u64 rodata_va, size_t rodata_sz,
                          u64 data_va,   size_t data_sz);
void  mmu_tlb_flush_all(void);
void  mmu_tlb_flush_page(u64 va);
void *phys_to_virt_early(phys_addr_t pa);
phys_addr_t virt_to_phys_any(const void *va);
bool  mmu_is_on(void);
void  mmu_pmm_ready(void);     /* page tables may now be taken from the PMM */
phys_addr_t mmu_kernel_phys_base(void);
u64   mmu_virt_delta(void);    /* KERNEL_IMAGE_VMA - physical load address */

#endif /* _MONOX_MMU_H */
