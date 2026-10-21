/* MONOX Kernel - Physical Memory Manager (Buddy Allocator)
 *
 * Features:
 *  - Buddy allocator up to order PMM_MAX_ORDER-1
 *  - Memory zones: ZONE_DMA, ZONE_NORMAL, ZONE_GPU
 *  - If RAM > 4 GiB: ZONE_GPU carved at 4 GiB boundary for GPU DMA
 *  - Zero allocation on free (security)
 *  - Per-zone statistics
 *
 * Bug-fix pass summary (see CHANGELOG-BUGFIX.md):
 *  - PAGE_MASK was undefined here (compile error).
 *  - pmm_init() wrote free-list headers *over the kernel image, the DTB and
 *    firmware carve-outs* because reservations were applied afterwards (and
 *    pmm_reserve_range() did not even work). Reservations are now a table
 *    consulted while the free lists are built.
 *  - Zones tracked "base + summed sizes", so RAM after a hole fell outside
 *    the zone and could never be freed (permanent leak) - zones now track
 *    their real span.
 *  - RAM between 4 and 8 GiB was silently dropped when total RAM <= 4 GiB.
 *  - Buddy was computed relative to the zone base but blocks were aligned
 *    to absolute addresses -> misaligned merges. Now absolute.
 *  - Split path could hand out an oversized block (leaking the tail).
 *  - Added double-free detection and whole-block-in-zone checks.
 *  - All locks are IRQ-safe.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "pmm.h"
#include "../arch/arm64/mm/mmu.h"

/* -------------------------------------------------------
 * Constants
 * -------------------------------------------------------*/
#define PAGE_SIZE        4096UL
#define PAGE_SHIFT       12
#define PAGE_MASK        (~(PAGE_SIZE - 1))
#define MAX_ORDER        PMM_MAX_ORDER
#define PAGES_PER_ORDER(o) (1UL << (o))

#define PMM_FREE_MAGIC   0x46524545U   /* "FREE" */

#define ZONE_DMA_MAX_SIZE    (4ULL * 1024 * 1024 * 1024)  /* 4 GiB */

typedef enum {
    ZONE_DMA    = 0,   /* < 4 GiB: accessible by all DMA masters */
    ZONE_NORMAL = 1,   /* >= 8 GiB (or 4..8 GiB when no GPU split) */
    ZONE_GPU    = 2,   /* [4 GiB, 8 GiB) when RAM > 4 GiB         */
    ZONE_MAX    = 3
} zone_id_t;

struct free_page {
    struct list_head  list;
    u32               order;
    u32               zone;
    u32               magic;
};

struct buddy_zone {
    spinlock_t        lock;
    struct list_head  free_list[MAX_ORDER];
    u64               free_pages;
    u64               total_pages;   /* managed (non-reserved) pages */
    phys_addr_t       base;          /* span start                   */
    size_t            size;          /* span length (end - base)     */
    const char       *name;
    bool              active;
};

static struct buddy_zone g_zones[ZONE_MAX];
static bool g_pmm_inited = false;

/* -------------------------------------------------------
 * Reserved ranges (kernel image, DTB, firmware carve-outs...)
 * -------------------------------------------------------*/
#define MAX_RESERVED 128
static struct { phys_addr_t base; phys_addr_t end; } g_resv[MAX_RESERVED];
static u32 g_nr_resv = 0;
static spinlock_t g_resv_lock = SPIN_LOCK_INIT;

static bool range_reserved(phys_addr_t pa, size_t len)
{
    phys_addr_t end = pa + len;
    for (u32 i = 0; i < g_nr_resv; i++) {
        if (pa < g_resv[i].end && end > g_resv[i].base)
            return true;
    }
    return false;
}

/* -------------------------------------------------------
 * Helpers
 * -------------------------------------------------------*/
static __always_inline phys_addr_t node_to_pa(const struct free_page *fp)
{
    return (phys_addr_t)((u64)fp - KERNEL_VA_BASE);
}

static __always_inline struct free_page *pa_to_node(phys_addr_t pa)
{
    return (struct free_page *)(pa + KERNEL_VA_BASE);
}

static __always_inline bool page_in_zone(const struct buddy_zone *z, phys_addr_t pa)
{
    return z->size && pa >= z->base && pa < z->base + z->size;
}

static zone_id_t zone_for_page(phys_addr_t pa)
{
    for (int i = 0; i < ZONE_MAX; i++) {
        if (g_zones[i].active && page_in_zone(&g_zones[i], pa))
            return (zone_id_t)i;
    }
    return ZONE_MAX; /* invalid */
}

/* Absolute-address buddy: blocks are naturally aligned in absolute terms
 * (see free_pages_to_zone), so the partner is simply pa ^ block_size. */
static __always_inline phys_addr_t buddy_addr(phys_addr_t pa, u32 order)
{
    return pa ^ ((u64)PAGE_SIZE << order);
}

static void zone_extend(struct buddy_zone *z, phys_addr_t base, phys_addr_t end)
{
    if (!z->active) {
        z->base   = base;
        z->size   = (size_t)(end - base);
        z->active = true;
        return;
    }
    phys_addr_t zb = MIN(z->base, base);
    phys_addr_t ze = MAX(z->base + z->size, end);
    z->base = zb;
    z->size = (size_t)(ze - zb);
}

/* Insert one free block (caller holds the zone lock if the zone is live) */
static void zone_insert_block(struct buddy_zone *z, phys_addr_t pa, u32 order)
{
    struct free_page *fp = pa_to_node(pa);
    memset(fp, 0, sizeof(*fp));
    list_init(&fp->list);
    fp->order = order;
    fp->zone  = (u32)(z - g_zones);
    fp->magic = PMM_FREE_MAGIC;
    list_add(&fp->list, &z->free_list[order]);
    z->free_pages += PAGES_PER_ORDER(order);
}

/* -------------------------------------------------------
 * free_pages_to_zone - add a range of free pages to a zone, skipping every
 * reserved sub-range, always using the largest naturally aligned blocks.
 * -------------------------------------------------------*/
static void free_pages_to_zone(struct buddy_zone *z, phys_addr_t base, size_t size)
{
    phys_addr_t pa  = ALIGN(base, PAGE_SIZE);
    phys_addr_t end = (base + size) & PAGE_MASK;

    while (pa < end) {
        u32 order = MAX_ORDER - 1;
        while (order > 0) {
            u64 block = PAGE_SIZE << order;
            if ((pa & (block - 1)) == 0 && pa + block <= end &&
                !range_reserved(pa, block))
                break;
            order--;
        }

        if (order == 0 && range_reserved(pa, PAGE_SIZE)) {
            pa += PAGE_SIZE;               /* reserved page: never touch */
            continue;
        }

        zone_insert_block(z, pa, order);
        z->total_pages += PAGES_PER_ORDER(order);
        pa += PAGE_SIZE << order;
    }
}

/* -------------------------------------------------------
 * pmm_init - initialise physical memory manager
 *   Reserve the kernel image / DTB / firmware regions with
 *   pmm_reserve_range() BEFORE calling this.
 * -------------------------------------------------------*/
void pmm_init(const struct mem_region *regions, u32 nr_regions,
              u64 total_ram_bytes)
{
    if (g_pmm_inited) {
        pr_warn("PMM: pmm_init called twice, ignoring\n");
        return;
    }

    for (int i = 0; i < ZONE_MAX; i++) {
        spin_lock_init(&g_zones[i].lock);
        for (int o = 0; o < MAX_ORDER; o++)
            list_init(&g_zones[i].free_list[o]);
        g_zones[i].free_pages  = 0;
        g_zones[i].total_pages = 0;
        g_zones[i].active      = false;
        g_zones[i].base        = 0;
        g_zones[i].size        = 0;
    }

    g_zones[ZONE_DMA   ].name = "DMA";
    g_zones[ZONE_NORMAL].name = "NORMAL";
    g_zones[ZONE_GPU   ].name = "GPU";

    /*
     * Split rule:
     *  total RAM > 4 GiB -> [4,8) GiB is ZONE_GPU, >= 8 GiB is ZONE_NORMAL
     *  otherwise         -> everything below 4 GiB is ZONE_DMA and anything
     *                       at/above 4 GiB (RAM that merely *starts* high)
     *                       is ZONE_NORMAL (it used to be lost).
     */
    bool split_gpu = (total_ram_bytes > ZONE_DMA_MAX_SIZE);
    phys_addr_t normal_start = split_gpu ? 2 * ZONE_DMA_MAX_SIZE : ZONE_DMA_MAX_SIZE;

    for (u32 r = 0; r < nr_regions; r++) {
        phys_addr_t base = regions[r].base;
        size_t      size = regions[r].size;

        if (!size || regions[r].type != MEM_TYPE_RAM)
            continue;

        phys_addr_t region_end = base + size;

        /* Portion below 4 GiB -> ZONE_DMA */
        if (base < ZONE_DMA_MAX_SIZE) {
            phys_addr_t dma_end = MIN(region_end, ZONE_DMA_MAX_SIZE);
            zone_extend(&g_zones[ZONE_DMA], base, dma_end);
            free_pages_to_zone(&g_zones[ZONE_DMA], base, (size_t)(dma_end - base));
        }

        /* Portion [4 GiB .. 8 GiB) -> ZONE_GPU (if split enabled) */
        if (split_gpu && region_end > ZONE_DMA_MAX_SIZE) {
            phys_addr_t gpu_base = MAX(base, ZONE_DMA_MAX_SIZE);
            phys_addr_t gpu_end  = MIN(region_end, 2 * ZONE_DMA_MAX_SIZE);
            if (gpu_end > gpu_base) {
                zone_extend(&g_zones[ZONE_GPU], gpu_base, gpu_end);
                free_pages_to_zone(&g_zones[ZONE_GPU], gpu_base,
                                   (size_t)(gpu_end - gpu_base));
            }
        }

        /* Remainder -> ZONE_NORMAL */
        phys_addr_t norm_base = MAX(base, normal_start);
        if (region_end > norm_base) {
            zone_extend(&g_zones[ZONE_NORMAL], norm_base, region_end);
            free_pages_to_zone(&g_zones[ZONE_NORMAL], norm_base,
                               (size_t)(region_end - norm_base));
        }
    }

    g_pmm_inited = true;
    mmu_pmm_ready();

    for (int i = 0; i < ZONE_MAX; i++) {
        if (!g_zones[i].active) continue;
        pr_info("PMM: ZONE_%-6s base=0x%012llx span=%6llu MiB  free=%6llu pages\n",
                g_zones[i].name,
                g_zones[i].base,
                (u64)g_zones[i].size / (1024 * 1024),
                g_zones[i].free_pages);
    }
}

/* -------------------------------------------------------
 * __alloc_pages_zone - allocate 2^order contiguous pages from zone
 * -------------------------------------------------------*/
static phys_addr_t __alloc_pages_zone(struct buddy_zone *z, u32 order)
{
    u64 flags;
    spin_lock_irqsave(&z->lock, flags);

    for (u32 o = order; o < MAX_ORDER; o++) {
        if (list_empty(&z->free_list[o]))
            continue;

        struct list_head *entry = z->free_list[o].next;
        list_del(entry);
        struct free_page *fp = list_entry(entry, struct free_page, list);
        phys_addr_t pa = node_to_pa(fp);
        z->free_pages -= PAGES_PER_ORDER(o);

        /* Split down to the requested order; the upper halves go back. The
         * upper half of a free block always lies inside the zone, so the
         * old "can't split near the zone edge" escape (which leaked the
         * tail of the block) is gone. */
        while (o > order) {
            o--;
            zone_insert_block(z, pa + (PAGE_SIZE << o), o);
        }

        fp->magic = 0;                 /* no longer a free block */
        fp->list.next = fp->list.prev = NULL;
        spin_unlock_irqrestore(&z->lock, flags);
        return pa;
    }

    spin_unlock_irqrestore(&z->lock, flags);
    return 0; /* failure */
}

/* -------------------------------------------------------
 * pmm_alloc_pages - allocate 2^order pages
 *   flags: GFP_KERNEL, GFP_DMA, GFP_GPU
 * -------------------------------------------------------*/
phys_addr_t pmm_alloc_pages(u32 order, gfp_t flags)
{
    if (order >= MAX_ORDER || !g_pmm_inited) return 0;

    zone_id_t preferred = ZONE_DMA;
    if (flags & GFP_GPU)
        preferred = ZONE_GPU;
    else if (!(flags & GFP_DMA) && g_zones[ZONE_NORMAL].active)
        preferred = ZONE_NORMAL;

    static const zone_id_t fallback[ZONE_MAX][ZONE_MAX] = {
        [ZONE_DMA   ] = { ZONE_DMA,    ZONE_NORMAL, ZONE_GPU    },
        [ZONE_NORMAL] = { ZONE_NORMAL, ZONE_DMA,    ZONE_GPU    },
        [ZONE_GPU   ] = { ZONE_GPU,    ZONE_DMA,    ZONE_NORMAL },
    };

    for (int i = 0; i < ZONE_MAX; i++) {
        zone_id_t z = fallback[preferred][i];
        if (!g_zones[z].active) continue;
        /* GFP_DMA must stay inside the DMA zone */
        if ((flags & GFP_DMA) && z != ZONE_DMA) continue;
        phys_addr_t pa = __alloc_pages_zone(&g_zones[z], order);
        if (pa) {
            if (flags & __GFP_ZERO)
                memset(phys_to_virt_early(pa), 0, PAGE_SIZE << order);
            return pa;
        }
    }

    pr_err("PMM: out of memory! order=%u flags=0x%x\n", order, flags);
    return 0;
}

/* -------------------------------------------------------
 * pmm_free_pages - return 2^order pages at pa to buddy
 * -------------------------------------------------------*/
void pmm_free_pages(phys_addr_t pa, u32 order)
{
    if (!pa || order >= MAX_ORDER) return;
    if (!IS_ALIGNED(pa, PAGE_SIZE)) {
        pr_err("PMM: free_pages unaligned pa=0x%llx\n", pa);
        return;
    }

    zone_id_t zid = zone_for_page(pa);
    if (zid >= ZONE_MAX) {
        pr_err("PMM: free_pages pa=0x%llx not in any zone!\n", pa);
        return;
    }
    struct buddy_zone *z = &g_zones[zid];

    /* the whole block must be inside the zone span */
    if (!page_in_zone(z, pa + (PAGE_SIZE << order) - 1)) {
        pr_err("PMM: free_pages block pa=0x%llx order=%u crosses zone end\n", pa, order);
        return;
    }

    u64 flags;
    spin_lock_irqsave(&z->lock, flags);

    /* Double free of the head page? (header is only ever present on blocks
     * that are on a free list) */
    {
        struct free_page *head = pa_to_node(pa);
        if (head->magic == PMM_FREE_MAGIC && head->list.next && head->list.prev) {
            spin_unlock_irqrestore(&z->lock, flags);
            pr_err("PMM: DOUBLE FREE detected pa=0x%llx order=%u\n", pa, order);
            return;
        }
    }

    /* Zero the block for security (done under the lock only for the
     * header-sensitive part would be pointless; the block is ours). */
    memset(pa_to_node(pa), 0, PAGE_SIZE << order);

    /* Coalesce with buddy as long as possible */
    while (order < MAX_ORDER - 1) {
        phys_addr_t buddy_pa = buddy_addr(pa, order);

        if (!page_in_zone(z, buddy_pa) ||
            !page_in_zone(z, buddy_pa + (PAGE_SIZE << order) - 1))
            break;

        /* Is the buddy the head of a free block of exactly this order?
         * (pointer comparison only - never dereferences the buddy, which
         * might be in a hole of the zone span) */
        struct free_page *buddy_fp = pa_to_node(buddy_pa);
        bool found = false;
        struct list_head *pos;
        list_for_each(pos, &z->free_list[order]) {
            if (pos == &buddy_fp->list) { found = true; break; }
        }
        if (!found) break;

        list_del(&buddy_fp->list);
        z->free_pages -= PAGES_PER_ORDER(order);
        /* scrub the stale header of the absorbed buddy */
        memset(buddy_fp, 0, sizeof(*buddy_fp));

        if (buddy_pa < pa) pa = buddy_pa;
        order++;
    }

    zone_insert_block(z, pa, order);
    spin_unlock_irqrestore(&z->lock, flags);
}

/* -------------------------------------------------------
 * pmm_reserve_range - keep [base, base+size) out of the allocator.
 * Before pmm_init(): just recorded (nothing is ever written there).
 * After pmm_init(): every free block overlapping the range is taken off
 * its list and re-added minus the reserved part.
 * -------------------------------------------------------*/
void pmm_reserve_range(phys_addr_t base, size_t size)
{
    if (!size) return;

    phys_addr_t rb = base & PAGE_MASK;
    phys_addr_t re = ALIGN(base + size, PAGE_SIZE);

    u64 rf;
    spin_lock_irqsave(&g_resv_lock, rf);
    if (g_nr_resv >= MAX_RESERVED) {
        spin_unlock_irqrestore(&g_resv_lock, rf);
        pr_err("PMM: reserved-range table full, cannot reserve 0x%llx+0x%zx\n",
               base, size);
        return;
    }
    g_resv[g_nr_resv].base = rb;
    g_resv[g_nr_resv].end  = re;
    g_nr_resv++;
    spin_unlock_irqrestore(&g_resv_lock, rf);

    if (!g_pmm_inited)
        return;

    /* Late reservation: carve it out of the free lists. */
    for (int zi = 0; zi < ZONE_MAX; zi++) {
        struct buddy_zone *z = &g_zones[zi];
        if (!z->active) continue;

        bool again = true;
        while (again) {
            again = false;
            u64 flags;
            spin_lock_irqsave(&z->lock, flags);
            for (u32 o = 0; o < MAX_ORDER && !again; o++) {
                struct list_head *pos;
                list_for_each(pos, &z->free_list[o]) {
                    struct free_page *fp = list_entry(pos, struct free_page, list);
                    phys_addr_t bpa = node_to_pa(fp);
                    u64 bsz = PAGE_SIZE << o;
                    if (!(bpa < re && bpa + bsz > rb))
                        continue;

                    list_del(pos);
                    z->free_pages  -= PAGES_PER_ORDER(o);
                    z->total_pages -= PAGES_PER_ORDER(o);
                    memset(fp, 0, sizeof(*fp));
                    /* re-add everything except the reserved part */
                    free_pages_to_zone(z, bpa, (size_t)bsz);
                    again = true;
                    break;
                }
            }
            spin_unlock_irqrestore(&z->lock, flags);
        }
    }
}

/* -------------------------------------------------------
 * pmm_stats - print memory stats
 * -------------------------------------------------------*/
void pmm_stats(void)
{
    for (int i = 0; i < ZONE_MAX; i++) {
        if (!g_zones[i].active) continue;
        u64 free_kb = g_zones[i].free_pages * 4;
        u64 tot_kb  = g_zones[i].total_pages * 4;
        pr_info("PMM: [%s] total=%llu KiB  free=%llu KiB  used=%llu KiB\n",
                g_zones[i].name, tot_kb, free_kb, tot_kb - free_kb);
    }
}

/* -------------------------------------------------------
 * pmm_gpu_zone_base / size - for GPU driver
 * -------------------------------------------------------*/
phys_addr_t pmm_gpu_zone_base(void)  { return g_zones[ZONE_GPU].base; }
size_t      pmm_gpu_zone_size(void)  { return g_zones[ZONE_GPU].size; }

/* -------------------------------------------------------
 * virt_to_phys_pmm / phys_to_virt_pmm: linear-map conversions.
 * (used to bounce through __virt_to_phys in main.c with a hard-coded
 *  constant; they now live next to the allocator and are consistent with
 *  node_to_pa()/pa_to_node())
 * -------------------------------------------------------*/
phys_addr_t virt_to_phys_pmm(const void *va)
{
    u64 a = (u64)va;
    return (a >= KERNEL_VA_BASE) ? a - KERNEL_VA_BASE : a;
}

void *phys_to_virt_pmm(phys_addr_t pa)
{
    return (void *)(pa + KERNEL_VA_BASE);
}
