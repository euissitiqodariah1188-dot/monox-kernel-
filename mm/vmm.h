/* MONOX Kernel - Virtual Memory Manager Interface */

#ifndef _MONOX_VMM_H
#define _MONOX_VMM_H

#include <kernel/types.h>

/* Kmalloc / slab */
void  kmalloc_init(void);
void *kmalloc(size_t sz);
void *kzalloc(size_t sz);
void  kfree(void *ptr);
void  kfree_pages(void *ptr, u32 order);
void *krealloc(void *ptr, size_t new_sz);
void  kmalloc_stats(void);

/* VMM: map physical pages into kernel virtual space */
struct vm_region {
    u64    va_start;
    u64    va_end;
    u64    pa_start;
    u32    flags;
    struct list_head node;
};

#define VM_READ    0x01
#define VM_WRITE   0x02
#define VM_EXEC    0x04
#define VM_DEVICE  0x08

int    vmm_init(void);
u64    vmm_alloc_pages(u32 order, u32 vm_flags);
void   vmm_free_region(u64 va);
u64    vmm_map_phys(phys_addr_t pa, size_t sz, u32 vm_flags);
void   vmm_unmap(u64 va, size_t sz);
void  *ioremap(phys_addr_t pa, size_t sz);
void   iounmap(void *va);

#endif /* _MONOX_VMM_H */
