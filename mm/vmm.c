/* MONOX Kernel - Virtual Memory Manager
 *
 * Manages the kernel virtual address space above KERNEL_VA_BASE.
 * Provides:
 *   - vmm_alloc_pages: alloc physical pages and map into kernel VA
 *   - vmm_map_phys:    map a physical range (MMIO) into kernel VA
 *   - ioremap / iounmap
 *   - vmm_free_region: unmap and free
 *
 * Bug-fix pass: vmm_free_region()/vmm_unmap() only flushed the TLB and
 * left the PTEs valid (use-after-free window), kernel VA was a pure bump
 * allocator (never recycled, so every task create/exit leaked VA *and* page
 * tables until the table pool was exhausted), region_add() failures were
 * ignored (region untrackable => pages unfreeable), mappings leaked on
 * error paths, zero-length MMIO maps returned an unmapped VA, and the
 * allocator locks were not IRQ-safe.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "vmm.h"
#include "pmm.h"
#include "../arch/arm64/mm/mmu.h"

/* -------------------------------------------------------
 * Kernel virtual address allocator
 * Bump allocator + a small free list so freed ranges (task stacks, ioremap
 * windows, ...) are reused instead of leaking VA and page tables.
 * -------------------------------------------------------*/
#define KVAS_START   0xFFFF800000000000ULL   /* kernel VA space start */
#define KVAS_END     0xFFFFFF8000000000ULL   /* kernel VA space end   */
#define KVAS_GUARD   4096ULL

static u64        g_kvas_next = KVAS_START;
static spinlock_t g_kvas_lock = SPIN_LOCK_INIT;

#define KVAS_FREE_SLOTS 256
static struct { u64 va; u64 size; } g_kvas_free[KVAS_FREE_SLOTS];  /* size==0: unused */

/* -------------------------------------------------------
 * Region tracking
 * -------------------------------------------------------*/
#define MAX_VM_REGIONS 1024

static struct vm_region g_regions[MAX_VM_REGIONS];
static u32              g_nr_regions = 0;
static spinlock_t       g_region_lock = SPIN_LOCK_INIT;

/* -------------------------------------------------------
 * vmm_init
 * -------------------------------------------------------*/
int vmm_init(void)
{
    spin_lock_init(&g_kvas_lock);
    spin_lock_init(&g_region_lock);
    memset(g_regions, 0, sizeof(g_regions));
    memset(g_kvas_free, 0, sizeof(g_kvas_free));
    g_kvas_next = KVAS_START;
    pr_info("VMM: kernel VA space [0x%llx .. 0x%llx]\n", KVAS_START, KVAS_END);
    return MX_OK;
}

/* -------------------------------------------------------
 * kvas_alloc - allocate a range in kernel virtual space
 * (each range is followed by an unmapped 4 KiB guard page)
 * -------------------------------------------------------*/
static u64 kvas_alloc(size_t size)
{
    size = ALIGN(size, 4096);
    u64 need = (u64)size + KVAS_GUARD;
    u64 flags;

    spin_lock_irqsave(&g_kvas_lock, flags);

    /* 1) reuse a freed range (best fit) */
    int best = -1;
    for (int i = 0; i < KVAS_FREE_SLOTS; i++) {
        if (g_kvas_free[i].size >= need &&
            (best < 0 || g_kvas_free[i].size < g_kvas_free[best].size))
            best = i;
    }
    if (best >= 0) {
        u64 va = g_kvas_free[best].va;
        g_kvas_free[best].va   += need;
        g_kvas_free[best].size -= need;
        spin_unlock_irqrestore(&g_kvas_lock, flags);
        return va;
    }

    /* 2) bump */
    if (g_kvas_next + need > KVAS_END) {
        spin_unlock_irqrestore(&g_kvas_lock, flags);
        return 0;
    }
    u64 va = g_kvas_next;
    g_kvas_next += need;
    spin_unlock_irqrestore(&g_kvas_lock, flags);
    return va;
}

/* kvas_free - give a range (size without guard) back */
static void kvas_free(u64 va, size_t size)
{
    u64 need = (u64)ALIGN(size, 4096) + KVAS_GUARD;
    u64 flags;

    spin_lock_irqsave(&g_kvas_lock, flags);

    /* top of the bump area: just roll it back */
    if (va + need == g_kvas_next) {
        g_kvas_next = va;
        spin_unlock_irqrestore(&g_kvas_lock, flags);
        return;
    }

    /* merge with an adjacent free range */
    for (int i = 0; i < KVAS_FREE_SLOTS; i++) {
        if (!g_kvas_free[i].size) continue;
        if (g_kvas_free[i].va + g_kvas_free[i].size == va) {
            g_kvas_free[i].size += need;
            spin_unlock_irqrestore(&g_kvas_lock, flags);
            return;
        }
        if (va + need == g_kvas_free[i].va) {
            g_kvas_free[i].va    = va;
            g_kvas_free[i].size += need;
            spin_unlock_irqrestore(&g_kvas_lock, flags);
            return;
        }
    }
    for (int i = 0; i < KVAS_FREE_SLOTS; i++) {
        if (!g_kvas_free[i].size) {
            g_kvas_free[i].va   = va;
            g_kvas_free[i].size = need;
            spin_unlock_irqrestore(&g_kvas_lock, flags);
            return;
        }
    }
    spin_unlock_irqrestore(&g_kvas_lock, flags);
    /* free list full: the VA is simply not reused (VA space is abundant) */
}

/* -------------------------------------------------------
 * region_add - track a mapped region
 * -------------------------------------------------------*/
static struct vm_region *region_add(u64 va, u64 pa, size_t size, u32 flags)
{
    u64 f;
    spin_lock_irqsave(&g_region_lock, f);
    for (u32 i = 0; i < MAX_VM_REGIONS; i++) {
        if (g_regions[i].va_start == 0) {
            g_regions[i].va_start = va;
            g_regions[i].va_end   = va + size;
            g_regions[i].pa_start = pa;
            g_regions[i].flags    = flags;
            g_nr_regions++;
            spin_unlock_irqrestore(&g_region_lock, f);
            return &g_regions[i];
        }
    }
    spin_unlock_irqrestore(&g_region_lock, f);
    return NULL;
}

/* region_take - find the region starting at va, copy it out and delete it
 * (atomically, so two racing frees cannot both release the same pages) */
static bool region_take(u64 va, struct vm_region *out)
{
    u64 f;
    spin_lock_irqsave(&g_region_lock, f);
    for (u32 i = 0; i < MAX_VM_REGIONS; i++) {
        if (g_regions[i].va_start == va) {
            *out = g_regions[i];
            memset(&g_regions[i], 0, sizeof(g_regions[i]));
            g_nr_regions--;
            spin_unlock_irqrestore(&g_region_lock, f);
            return true;
        }
    }
    spin_unlock_irqrestore(&g_region_lock, f);
    return false;
}

/* -------------------------------------------------------
 * PTE flags from vm_flags
 * -------------------------------------------------------*/
#define PTE_VALID      (1ULL << 0)
#define PTE_PAGE       (1ULL << 1)
#define PTE_AF         (1ULL << 10)
#define PTE_SH_INNER   (3ULL << 8)
#define PTE_SH_OUTER   (2ULL << 8)
#define PTE_UXN        (1ULL << 54)
#define PTE_PXN        (1ULL << 53)
#define PTE_RO         (1ULL << 7)
#define PTE_ATTR_NORM  (3ULL << 2)
#define PTE_ATTR_DEV   (0ULL << 2)

static u64 vm_flags_to_pte(u32 flags)
{
    u64 pte = PTE_VALID | PTE_PAGE | PTE_AF;
    if (flags & VM_DEVICE)
        pte |= PTE_SH_OUTER | PTE_ATTR_DEV | PTE_UXN | PTE_PXN;
    else
        pte |= PTE_SH_INNER | PTE_ATTR_NORM | PTE_UXN;
    if (!(flags & VM_WRITE))
        pte |= PTE_RO;
    if (!(flags & VM_EXEC))
        pte |= PTE_PXN;
    return pte;
}

static u32 size_to_order(size_t size)
{
    u32 order = 0;
    size_t sz = 4096;
    while (sz < size && order < PMM_MAX_ORDER) { sz <<= 1; order++; }
    return order;
}

/* -------------------------------------------------------
 * vmm_alloc_pages - allocate physical pages and map into kernel VA
 * Returns kernel virtual address, or 0 on failure
 * -------------------------------------------------------*/
u64 vmm_alloc_pages(u32 order, u32 vm_flags)
{
    if (order >= PMM_MAX_ORDER) return 0;
    size_t size = (size_t)4096 << order;

    /* Allocate physical pages */
    phys_addr_t pa = pmm_alloc_pages(order, GFP_KERNEL | __GFP_ZERO);
    if (!pa) return 0;

    /* Allocate kernel VA */
    u64 va = kvas_alloc(size);
    if (!va) {
        pmm_free_pages(pa, order);
        return 0;
    }

    /* Map in page tables */
    u64 pte_flags = vm_flags_to_pte(vm_flags);
    int ret = mmu_map_range(va, pa, size, pte_flags);
    if (ret != MX_OK) {
        mmu_unmap_range(va, size);
        kvas_free(va, size);
        pmm_free_pages(pa, order);
        return 0;
    }

    /* An untracked region could never be freed again => undo everything */
    if (!region_add(va, pa, size, vm_flags)) {
        pr_err("VMM: region table full\n");
        mmu_unmap_range(va, size);
        kvas_free(va, size);
        pmm_free_pages(pa, order);
        return 0;
    }
    return va;
}

/* -------------------------------------------------------
 * vmm_free_region - unmap and free a vmm_alloc_pages region
 * -------------------------------------------------------*/
void vmm_free_region(u64 va)
{
    struct vm_region r;
    if (!region_take(va, &r)) {
        pr_warn("VMM: vmm_free_region: no region at 0x%llx\n", va);
        return;
    }

    size_t size = (size_t)(r.va_end - r.va_start);

    /* Unmap FIRST (also flushes the TLB), then release the pages: the old
     * code left the PTEs valid, so a stale pointer kept access to memory
     * that had already been handed to someone else. */
    mmu_unmap_range(r.va_start, size);

    if (!(r.flags & VM_DEVICE))
        pmm_free_pages(r.pa_start, size_to_order(size));

    kvas_free(r.va_start, size);
}

/* -------------------------------------------------------
 * vmm_map_phys - map a physical address range (MMIO) into kernel VA
 * Returns kernel virtual address, or 0 on failure
 * -------------------------------------------------------*/
u64 vmm_map_phys(phys_addr_t pa, size_t size, u32 vm_flags)
{
    phys_addr_t pa_aligned = pa & ~(phys_addr_t)(4096 - 1);
    size_t offset = (size_t)(pa - pa_aligned);
    size_t total  = ALIGN(size + offset, 4096);
    if (total == 0) total = 4096;          /* never hand out an unmapped VA */

    u64 va = kvas_alloc(total);
    if (!va) return 0;

    u64 pte_flags = vm_flags_to_pte(vm_flags | VM_DEVICE);
    int ret = mmu_map_range(va, pa_aligned, total, pte_flags);
    if (ret != MX_OK) {
        mmu_unmap_range(va, total);
        kvas_free(va, total);
        return 0;
    }

    if (!region_add(va, pa_aligned, total, vm_flags | VM_DEVICE)) {
        pr_err("VMM: region table full (map_phys)\n");
        mmu_unmap_range(va, total);
        kvas_free(va, total);
        return 0;
    }
    return va + offset;
}

/* -------------------------------------------------------
 * vmm_unmap - remove a mapping created by vmm_map_phys (does not free
 * physical memory). 'size' is accepted for API compatibility; the tracked
 * region is authoritative.
 * -------------------------------------------------------*/
void vmm_unmap(u64 va, size_t size)
{
    (void)size;
    u64 va_aligned = va & ~(u64)(4096 - 1);
    struct vm_region r;

    if (!region_take(va_aligned, &r)) {
        pr_warn("VMM: vmm_unmap: no region at 0x%llx\n", va_aligned);
        return;
    }
    size_t rsz = (size_t)(r.va_end - r.va_start);
    mmu_unmap_range(r.va_start, rsz);
    kvas_free(r.va_start, rsz);
}

/* -------------------------------------------------------
 * ioremap / iounmap - convenience wrappers for device MMIO
 * -------------------------------------------------------*/
void *ioremap(phys_addr_t pa, size_t size)
{
    u64 va = vmm_map_phys(pa, size, VM_READ | VM_WRITE | VM_DEVICE);
    return va ? (void *)va : NULL;
}

void iounmap(void *va)
{
    if (!va) return;
    vmm_unmap((u64)va & ~(u64)(4096 - 1), 0);
}
