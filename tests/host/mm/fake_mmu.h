#ifndef FAKE_MMU_H
#define FAKE_MMU_H
#include <kernel/types.h>
extern u64 g_test_va_base;
#define KERNEL_VA_BASE g_test_va_base
void mmu_pmm_ready(void);
void *phys_to_virt_early(phys_addr_t pa);
#endif
