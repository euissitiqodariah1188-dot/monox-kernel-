/* MONOX Kernel - Slab Allocator (kmalloc / kfree)
 *
 * Implements a slab allocator over the buddy PMM.
 * Size classes: 16,32,64,128,256,512,1024,2048 bytes
 * Larger allocations go directly to the buddy allocator and are tracked in
 * a registry so that kfree() can free them (they used to leak).
 * Zero-initialise on alloc (kzalloc), scrub on free (security).
 *
 * Bug-fix pass:
 *  - The 8-byte class stored a 16-byte list node in an 8-byte object: every
 *    free-list link overwrote the neighbouring object (heap corruption).
 *  - The 4096-byte class had room for ZERO objects (header filled the page)
 *    so kmalloc(2049..4096) hit BUG_ON(list_empty) and panicked.
 *  - kfree() of any block > 4 KiB printed an error and leaked it (DTB,
 *    bitmaps, file buffers...). Large blocks are now tracked and freed.
 *  - krealloc() over-read the source for large blocks.
 *  - Double-free / bad-pointer detection added; locks are IRQ-safe.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "pmm.h"
#include "vmm.h"

#define SLAB_MAGIC   0xA110CA7EU   /* anti-corruption canary */

/* -------------------------------------------------------
 * Size classes
 * -------------------------------------------------------*/
static const u32 SLAB_SIZES[] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};
#define NR_SLAB_CLASSES  (sizeof(SLAB_SIZES) / sizeof(SLAB_SIZES[0]))

/* -------------------------------------------------------
 * Slab header (at the start of each 4 KiB slab page)
 * -------------------------------------------------------*/
struct slab_hdr {
    u32              magic;
    u32              obj_size;      /* size of each object           */
    u32              obj_count;     /* total objects in this slab    */
    u32              free_count;    /* free objects                  */
    struct list_head free_list;     /* list of struct slab_obj       */
    struct list_head node;          /* link in cache's slab list     */
    struct slab_cache *cache;
};

/* -------------------------------------------------------
 * Object free-slot (stored in the free object itself)
 * -------------------------------------------------------*/
struct slab_obj {
    struct list_head list;
};

/* -------------------------------------------------------
 * Per-size slab cache
 * -------------------------------------------------------*/
struct slab_cache {
    u32              obj_size;
    spinlock_t       lock;
    struct list_head partial;    /* slabs with free space */
    struct list_head full;       /* fully-allocated slabs */
    u64              alloc_count;
    u64              free_count;
};

static struct slab_cache g_caches[NR_SLAB_CLASSES];

/* -------------------------------------------------------
 * Utility
 * -------------------------------------------------------*/
static u32 slab_class_for(size_t sz)
{
    for (u32 i = 0; i < NR_SLAB_CLASSES; i++) {
        if (sz <= SLAB_SIZES[i]) return i;
    }
    return NR_SLAB_CLASSES; /* large allocation */
}

static size_t slab_objs_per_page(u32 obj_size)
{
    /* Usable space = PAGE_SIZE - sizeof(slab_hdr), rounded to obj_size */
    size_t usable = 4096 - ALIGN(sizeof(struct slab_hdr), obj_size);
    return usable / obj_size;
}

/* -------------------------------------------------------
 * Large (buddy) allocation registry
 * Large blocks are page-aligned, slab objects never are (the slab header
 * occupies the start of the page) - so "is the pointer page-aligned?" is
 * an exact discriminator in kfree().
 * -------------------------------------------------------*/
struct large_alloc {
    struct list_head node;
    void            *ptr;
    size_t           bytes;     /* 4096 << order */
    u32              order;
};

static LIST_HEAD(g_large_list);
static spinlock_t g_large_lock = SPIN_LOCK_INIT;

static u32 large_order_for(size_t sz)
{
    u32 order = 0;
    while (((size_t)4096 << order) < sz && order < PMM_MAX_ORDER)
        order++;
    return order;
}

/* remove + return the registry entry for ptr, or NULL */
static struct large_alloc *large_take(void *ptr)
{
    u64 f;
    struct list_head *pos;
    spin_lock_irqsave(&g_large_lock, f);
    list_for_each(pos, &g_large_list) {
        struct large_alloc *la = list_entry(pos, struct large_alloc, node);
        if (la->ptr == ptr) {
            list_del(&la->node);
            spin_unlock_irqrestore(&g_large_lock, f);
            return la;
        }
    }
    spin_unlock_irqrestore(&g_large_lock, f);
    return NULL;
}

static size_t large_size_of(void *ptr)
{
    u64 f;
    size_t bytes = 0;
    struct list_head *pos;
    spin_lock_irqsave(&g_large_lock, f);
    list_for_each(pos, &g_large_list) {
        struct large_alloc *la = list_entry(pos, struct large_alloc, node);
        if (la->ptr == ptr) { bytes = la->bytes; break; }
    }
    spin_unlock_irqrestore(&g_large_lock, f);
    return bytes;
}

/* -------------------------------------------------------
 * slab_new - allocate a new slab page for a cache
 * -------------------------------------------------------*/
static struct slab_hdr *slab_new(struct slab_cache *c)
{
    phys_addr_t pa = pmm_alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!pa) return NULL;

    struct slab_hdr *hdr = (struct slab_hdr *)phys_to_virt_pmm(pa);
    hdr->magic     = SLAB_MAGIC;
    hdr->obj_size  = c->obj_size;
    hdr->obj_count = (u32)slab_objs_per_page(c->obj_size);
    hdr->free_count= hdr->obj_count;
    hdr->cache     = c;
    list_init(&hdr->free_list);
    list_init(&hdr->node);

    /* Add all objects to free list */
    u8 *obj_start = (u8 *)hdr + ALIGN(sizeof(struct slab_hdr), c->obj_size);
    for (u32 i = 0; i < hdr->obj_count; i++) {
        struct slab_obj *obj = (struct slab_obj *)(obj_start + i * c->obj_size);
        list_add_tail(&obj->list, &hdr->free_list);
    }

    return hdr;
}

/* -------------------------------------------------------
 * kmalloc_init - called once at boot
 * -------------------------------------------------------*/
void kmalloc_init(void)
{
    for (u32 i = 0; i < NR_SLAB_CLASSES; i++) {
        g_caches[i].obj_size = SLAB_SIZES[i];
        spin_lock_init(&g_caches[i].lock);
        list_init(&g_caches[i].partial);
        list_init(&g_caches[i].full);
        g_caches[i].alloc_count = 0;
        g_caches[i].free_count  = 0;

        /* Pre-allocate one slab per class */
        struct slab_hdr *hdr = slab_new(&g_caches[i]);
        if (hdr)
            list_add(&hdr->node, &g_caches[i].partial);
    }
    pr_info("SLAB: %u size classes initialised (16..2048 bytes)\n",
            (u32)NR_SLAB_CLASSES);
}

/* -------------------------------------------------------
 * kmalloc - allocate sz bytes, returns kernel virtual pointer
 * -------------------------------------------------------*/
void *kmalloc(size_t sz)
{
    if (!sz) return NULL;

    u32 cls = slab_class_for(sz);

    /* Large allocation: go straight to buddy PMM (tracked for kfree) */
    if (cls >= NR_SLAB_CLASSES) {
        u32 order = large_order_for(sz);
        if (((size_t)4096 << order) < sz)
            return NULL;                      /* bigger than the biggest block */

        struct large_alloc *la = kmalloc(sizeof(*la));
        if (!la) return NULL;

        phys_addr_t pa = pmm_alloc_pages(order, GFP_KERNEL);
        if (!pa) {
            kfree(la);
            return NULL;
        }
        la->ptr   = phys_to_virt_pmm(pa);
        la->order = order;
        la->bytes = (size_t)4096 << order;
        list_init(&la->node);

        u64 f;
        spin_lock_irqsave(&g_large_lock, f);
        list_add(&la->node, &g_large_list);
        spin_unlock_irqrestore(&g_large_lock, f);
        return la->ptr;
    }

    struct slab_cache *c = &g_caches[cls];
    u64 flags;
    spin_lock_irqsave(&c->lock, flags);

    /* Get a partial slab or allocate new */
    struct slab_hdr *hdr = NULL;
    if (!list_empty(&c->partial)) {
        hdr = list_entry(c->partial.next, struct slab_hdr, node);
    } else {
        hdr = slab_new(c);
        if (!hdr) {
            spin_unlock_irqrestore(&c->lock, flags);
            return NULL;
        }
        list_add(&hdr->node, &c->partial);
    }

    BUG_ON(hdr->magic != SLAB_MAGIC);
    BUG_ON(list_empty(&hdr->free_list));

    /* Take first free object */
    struct list_head *obj_entry = hdr->free_list.next;
    list_del(obj_entry);
    hdr->free_count--;

    if (hdr->free_count == 0) {
        /* Slab is now full: move to full list */
        list_del(&hdr->node);
        list_add(&hdr->node, &c->full);
    }

    c->alloc_count++;
    spin_unlock_irqrestore(&c->lock, flags);

    /* Do not zero here - caller uses kzalloc for zeroed memory */
    return (void *)obj_entry;
}

/* -------------------------------------------------------
 * kzalloc - allocate zeroed memory
 * -------------------------------------------------------*/
void *kzalloc(size_t sz)
{
    void *p = kmalloc(sz);
    if (p) memset(p, 0, sz);
    return p;
}

/* -------------------------------------------------------
 * kfree - free a kmalloc'd pointer
 * -------------------------------------------------------*/
void kfree(void *ptr)
{
    if (!ptr) return;

    /* Page-aligned => large buddy block (slab objects never are) */
    if (((uintptr_t)ptr & (4096UL - 1)) == 0) {
        struct large_alloc *la = large_take(ptr);
        if (!la) {
            pr_err("kfree: ptr=%p is page-aligned but not a known large allocation "
                   "(double free or bad pointer)\n", ptr);
            return;
        }
        phys_addr_t pa = virt_to_phys_pmm(la->ptr);
        u32 order = la->order;
        kfree(la);
        pmm_free_pages(pa, order);       /* zeroes the block */
        return;
    }

    /* Locate the slab header: base of the 4 KiB page */
    struct slab_hdr *hdr = (struct slab_hdr *)((uintptr_t)ptr & ~(4096UL - 1));

    if (hdr->magic != SLAB_MAGIC) {
        pr_err("kfree: ptr=%p has no slab magic (bad pointer)\n", ptr);
        return;
    }

    struct slab_cache *c = hdr->cache;
    BUG_ON(!c);
    BUG_ON(c->obj_size != hdr->obj_size);

    /* The pointer must be exactly an object boundary inside this slab */
    uintptr_t first = (uintptr_t)hdr + ALIGN(sizeof(struct slab_hdr), hdr->obj_size);
    uintptr_t off   = (uintptr_t)ptr - first;
    if ((uintptr_t)ptr < first ||
        off % hdr->obj_size != 0 ||
        off / hdr->obj_size >= hdr->obj_count) {
        pr_err("kfree: ptr=%p is not an object of its slab (corruption?)\n", ptr);
        return;
    }

    u64 flags;
    spin_lock_irqsave(&c->lock, flags);

    /* Double free: the object is already on this slab's free list */
    struct list_head *pos;
    list_for_each(pos, &hdr->free_list) {
        if (pos == (struct list_head *)ptr) {
            spin_unlock_irqrestore(&c->lock, flags);
            pr_err("kfree: DOUBLE FREE of %p detected\n", ptr);
            return;
        }
    }

    /* Scrub object memory (security: prevent use-after-free info leak) */
    memset(ptr, 0xDE, hdr->obj_size);

    bool was_full = (hdr->free_count == 0);

    struct slab_obj *obj = (struct slab_obj *)ptr;
    list_add(&obj->list, &hdr->free_list);
    hdr->free_count++;
    c->free_count++;

    if (was_full) {
        /* Move from full back to partial */
        list_del(&hdr->node);
        list_add(&hdr->node, &c->partial);
    }

    if (hdr->free_count == hdr->obj_count) {
        /* Slab is now completely empty: return page to buddy. (Checked
         * after the full->partial move so single-object slabs are also
         * released.) */
        list_del(&hdr->node);
        spin_unlock_irqrestore(&c->lock, flags);
        phys_addr_t pa = virt_to_phys_pmm(hdr);
        pmm_free_page(pa);                /* zeroes the page */
        return;
    }

    spin_unlock_irqrestore(&c->lock, flags);
}

/* -------------------------------------------------------
 * kfree_pages - free large buddy allocation
 * (kept for callers that allocated with pmm_alloc_pages directly; if the
 *  pointer is a tracked kmalloc block it is released through kfree)
 * -------------------------------------------------------*/
void kfree_pages(void *ptr, u32 order)
{
    if (!ptr) return;

    struct large_alloc *la = large_take(ptr);
    if (la) {
        order = la->order;
        kfree(la);
    }
    pmm_free_pages(virt_to_phys_pmm(ptr), order);   /* zeroes the block */
}

/* -------------------------------------------------------
 * krealloc - reallocate
 * -------------------------------------------------------*/
void *krealloc(void *ptr, size_t new_sz)
{
    if (!ptr) return kmalloc(new_sz);
    if (!new_sz) { kfree(ptr); return NULL; }

    size_t old_sz;
    if (((uintptr_t)ptr & (4096UL - 1)) == 0) {
        old_sz = large_size_of(ptr);
        if (!old_sz) {
            pr_err("krealloc: unknown block %p\n", ptr);
            return NULL;
        }
    } else {
        struct slab_hdr *hdr = (struct slab_hdr *)((uintptr_t)ptr & ~(4096UL - 1));
        if (hdr->magic != SLAB_MAGIC) {
            pr_err("krealloc: bad pointer %p\n", ptr);
            return NULL;
        }
        old_sz = hdr->obj_size;
    }

    if (new_sz <= old_sz && slab_class_for(new_sz) == slab_class_for(old_sz))
        return ptr;                       /* still fits the same class */

    void *new_ptr = kmalloc(new_sz);
    if (!new_ptr) return NULL;            /* old block untouched */

    memcpy(new_ptr, ptr, MIN(old_sz, new_sz));
    kfree(ptr);
    return new_ptr;
}

/* -------------------------------------------------------
 * kmalloc_stats
 * -------------------------------------------------------*/
void kmalloc_stats(void)
{
    for (u32 i = 0; i < NR_SLAB_CLASSES; i++) {
        pr_info("SLAB[%4u]: alloc=%llu free=%llu\n",
                SLAB_SIZES[i],
                g_caches[i].alloc_count,
                g_caches[i].free_count);
    }
}
