/* MONOX Kernel - ARM64 MMU / Page Table Management
 *
 * 4 KiB granule, 48-bit VA, 4-level page tables (PGD->PUD->PMD->PTE).
 *
 * One table hierarchy serves TTBR0 *and* TTBR1; walkers index with VA[47:0]
 * only. Three kinds of mapping live in it:
 *   - linear map  VA = KERNEL_VA_BASE + PA   (RAM + MMIO). Because
 *     VA[47:0] == PA, the very same entries are also the identity map the
 *     boot CPU needs while it still executes at the physical address;
 *   - kernel image at its link address KERNEL_IMAGE_VMA -> the physical
 *     address the loader used (read at run time), mapped RWX at first and
 *     hardened by mmu_protect_kernel();
 *   - the image's identity alias (needed to keep fetching instructions
 *     across the MMU switch).
 * After mmu_enable(), head.S's __enter_virt moves execution to the high
 * image alias; the identity entries stay valid for pointers created earlier.
 *
 * Bug-fix pass summary (see CHANGELOG-BUGFIX.md):
 *   - RAM and MMIO were never mapped (only the kernel image) => the very
 *     first PMM free-list write faulted. mmu_map_linear() added.
 *   - Tables were walked through high VAs *before* the MMU was on, and the
 *     TTBR value was computed from a physical pointer minus a virtual
 *     offset. Now every table access goes through phys_to_virt_early(),
 *     which is identity while the MMU is off.
 *   - A 2 MiB/1 GiB block descriptor was treated as a table pointer by the
 *     walker (writing PTEs over real memory). Blocks are now split.
 *   - mmu_unmap_range() added (vmm_free/vmm_unmap used to leave stale,
 *     still-valid mappings to freed pages).
 *   - Table pool exhaustion no longer panics at run time: tables can be
 *     taken from the PMM once it is up.
 *   - TCR.IPS/AS now follow what the CPU actually supports.
 *   - The image no longer has to be loaded at one fixed physical address
 *     (QEMU loads it at RAM base + 2 MiB).
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "mmu.h"
#include "../../../mm/pmm.h"

/* -------------------------------------------------------
 * Page size constants (4 KiB granule)
 * -------------------------------------------------------*/
#define PAGE_SHIFT      12
#define PAGE_SIZE       (1UL << PAGE_SHIFT)   /* 4096       */
#define PAGE_MASK       (~(PAGE_SIZE - 1))
#define PTRS_PER_PAGE   512                   /* 2^9        */
#define TABLE_SIZE      (PTRS_PER_PAGE * 8)   /* 4 KiB      */

#define PTE_SHIFT       (PAGE_SHIFT)
#define PMD_SHIFT       (PTE_SHIFT  + 9)      /* 21 */
#define PUD_SHIFT       (PMD_SHIFT  + 9)      /* 30 */
#define PGD_SHIFT       (PUD_SHIFT  + 9)      /* 39 */

#define PMD_SIZE        (1UL << PMD_SHIFT)    /* 2 MiB  */
#define PUD_SIZE        (1UL << PUD_SHIFT)    /* 1 GiB  */

#define PGD_INDEX(va)   (((va) >> PGD_SHIFT) & 0x1FF)
#define PUD_INDEX(va)   (((va) >> PUD_SHIFT) & 0x1FF)
#define PMD_INDEX(va)   (((va) >> PMD_SHIFT) & 0x1FF)
#define PTE_INDEX(va)   (((va) >> PTE_SHIFT) & 0x1FF)

/* -------------------------------------------------------
 * ARMv8/v9 descriptor bits
 * -------------------------------------------------------*/
#define PTE_VALID       (1ULL << 0)
#define PTE_TABLE       (1ULL << 1)   /* L0-L2: 1=table, 0=block */
#define PTE_PAGE        (1ULL << 1)   /* L3:    1=page           */

#define PTE_ATTR(n)     ((u64)(n) << 2)
#define ATTR_DEVICE_nGnRnE  0
#define ATTR_DEVICE_nGnRE   1
#define ATTR_NORMAL_NC      2
#define ATTR_NORMAL         3
#define ATTR_NORMAL_WT      4

#define PTE_SH_NONE     (0ULL << 8)
#define PTE_SH_OUTER    (2ULL << 8)
#define PTE_SH_INNER    (3ULL << 8)

#define PTE_AF          (1ULL << 10)
#define PTE_NG          (1ULL << 11)
#define PTE_RO          (1ULL << 7)
#define PTE_USER        (1ULL << 6)
#define PTE_PXN         (1ULL << 53)
#define PTE_UXN         (1ULL << 54)

/* output-address field of a descriptor: bits [47:12] */
#define PTE_ADDR_MASK   0x0000FFFFFFFFF000ULL

/* attribute bits that are copied when a block is split */
#define PTE_ATTR_MASK   (~PTE_ADDR_MASK & ~3ULL)

/* flag sets WITHOUT the type bits: the walker adds block/page type */
#define ATTRS_RWX    (PTE_AF|PTE_SH_INNER|PTE_ATTR(ATTR_NORMAL))
#define ATTRS_RW_NX  (ATTRS_RWX|PTE_PXN|PTE_UXN)
#define ATTRS_DEVICE (PTE_AF|PTE_SH_OUTER|PTE_ATTR(ATTR_DEVICE_nGnRnE)|PTE_PXN|PTE_UXN)

/* -------------------------------------------------------
 * MAIR_EL1
 *   0: Device nGnRnE  1: Device nGnRE  2: Normal NC
 *   3: Normal WB/WA   4: Normal WT
 * -------------------------------------------------------*/
#define MAIR_DEVICE_nGnRnE   0x00ULL
#define MAIR_DEVICE_nGnRE    0x04ULL
#define MAIR_NORMAL_NC       0x44ULL
#define MAIR_NORMAL_WB       0xFFULL
#define MAIR_NORMAL_WT       0xBBULL

#define MONOX_MAIR_VALUE    ( \
    (MAIR_DEVICE_nGnRnE << 0)  |   \
    (MAIR_DEVICE_nGnRE  << 8)  |   \
    (MAIR_NORMAL_NC     << 16) |   \
    (MAIR_NORMAL_WB     << 24) |   \
    (MAIR_NORMAL_WT     << 32)     \
)

/* -------------------------------------------------------
 * TCR_EL1 (IPS / AS are filled in at run time from the CPU ID regs)
 * -------------------------------------------------------*/
#define TCR_T0SZ(x)     ((u64)(x)       )
#define TCR_T1SZ(x)     ((u64)(x) << 16 )
#define TCR_TG0_4KB     (0ULL   << 14)
#define TCR_TG1_4KB     (2ULL   << 30)
#define TCR_SH0_INNER   (3ULL   << 12)
#define TCR_SH1_INNER   (3ULL   << 28)
#define TCR_ORGN0_WB    (1ULL   << 10)
#define TCR_IRGN0_WB    (1ULL   << 8 )
#define TCR_ORGN1_WB    (1ULL   << 26)
#define TCR_IRGN1_WB    (1ULL   << 24)
#define TCR_AS_16BIT    (1ULL   << 36)
#define TCR_HA          (1ULL   << 39)
#define TCR_HD          (1ULL   << 40)

#define MONOX_TCR_BASE (\
    TCR_T0SZ(16)    | TCR_T1SZ(16)    | \
    TCR_TG0_4KB     | TCR_TG1_4KB     | \
    TCR_SH0_INNER   | TCR_SH1_INNER   | \
    TCR_ORGN0_WB    | TCR_IRGN0_WB    | \
    TCR_ORGN1_WB    | TCR_IRGN1_WB      \
)

/* -------------------------------------------------------
 * State
 * -------------------------------------------------------*/
volatile u32 g_kernel_locks_enabled = 0;   /* set once the MMU is on */

extern volatile u64 secondary_mmu_regs[4]; /* head.S: {mair,tcr,ttbr,sctlr} */
extern u64 __kernel_phys_base;             /* head.S: physical load address */

static u64 __aligned(TABLE_SIZE) g_pgd_kernel[PTRS_PER_PAGE];
static spinlock_t g_pgd_lock = SPIN_LOCK_INIT;

#define EARLY_PT_POOL_TABLES 512
#define EARLY_PT_POOL_SIZE  (EARLY_PT_POOL_TABLES * TABLE_SIZE)   /* 2 MiB */
static u8 __aligned(TABLE_SIZE) g_early_pt_pool[EARLY_PT_POOL_SIZE];
static size_t g_early_pt_used = 0;

static bool   g_mmu_on      = false;
static bool   g_pmm_ready   = false;
static u64    g_kvirt       = KERNEL_IMAGE_VMA;  /* image VMA              */
static u64    g_kphys       = 0;                 /* image physical base    */
static size_t g_ksize       = 0;                 /* image size (bytes)     */

bool mmu_is_on(void) { return g_mmu_on; }
void mmu_pmm_ready(void) { g_pmm_ready = true; }
phys_addr_t mmu_kernel_phys_base(void) { return g_kphys; }
u64 mmu_virt_delta(void) { return g_kvirt - g_kphys; }

/* -------------------------------------------------------
 * Address translation helpers
 *
 *  - While the MMU is off every address the C code holds is physical.
 *  - After it is on, RAM/MMIO are reachable at KERNEL_VA_BASE + PA.
 * -------------------------------------------------------*/
void *phys_to_virt_early(phys_addr_t pa)
{
    if (g_mmu_on)
        return (void *)(pa + KERNEL_VA_BASE);
    return (void *)pa;
}

/* Convert ANY kernel pointer to a physical address:
 *   low address        -> already physical (identity alias / pre-MMU)
 *   kernel image VA    -> image offset
 *   linear-map VA      -> subtract KERNEL_VA_BASE
 * (the old code subtracted a single constant, which is wrong for statics
 *  when running at the low alias and wrong for the image if it is not
 *  loaded at one fixed physical address) */
phys_addr_t virt_to_phys_any(const void *ptr)
{
    u64 a = (u64)ptr;
    if (a < KERNEL_VA_BASE)
        return a;
    if (g_ksize && a >= g_kvirt && a < g_kvirt + g_ksize)
        return a - g_kvirt + g_kphys;
    return a - KERNEL_VA_BASE;
}

/* -------------------------------------------------------
 * Table allocation
 * -------------------------------------------------------*/
static u64 *alloc_table(void)
{
    u64 *table = NULL;

    if (g_early_pt_used + TABLE_SIZE <= EARLY_PT_POOL_SIZE) {
        table = (u64 *)(g_early_pt_pool + g_early_pt_used);
        g_early_pt_used += TABLE_SIZE;
    } else if (g_pmm_ready) {
        phys_addr_t pa = pmm_alloc_pages(0, GFP_KERNEL | __GFP_ZERO);
        if (pa)
            table = (u64 *)phys_to_virt_early(pa);
    }
    if (!table)
        return NULL;                 /* caller reports MX_ENOMEM */

    memset(table, 0, TABLE_SIZE);
    return table;
}

static inline u64 *table_from_desc(u64 d)
{
    return (u64 *)phys_to_virt_early(d & PTE_ADDR_MASK);
}

static inline u64 table_desc(u64 *tbl)
{
    return (virt_to_phys_any(tbl) & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
}

/* Break-before-make flush used when an existing descriptor is replaced */
static void flush_tlb_local_all(void)
{
    dsb(ishst);
    __asm__ volatile("tlbi vmalle1is" ::: "memory");
    dsb(ish);
    isb();
}

/* Return the next-level table for parent[idx], creating it if empty or
 * splitting an existing block descriptor into a table that reproduces the
 * same mapping. level: 2 = parent is PUD (child is PMD), 1 = parent is PMD
 * (child is PTE). For the PGD (level 3) blocks are not architecturally
 * valid. Returns NULL on out-of-memory. */
static u64 *next_table(u64 *parent, u64 idx, int level)
{
    u64 d = parent[idx];

    if (!(d & PTE_VALID)) {
        u64 *t = alloc_table();
        if (!t) return NULL;
        dsb(ishst);
        parent[idx] = table_desc(t);
        return t;
    }

    if ((d & 3) == 3)                       /* already a table */
        return table_from_desc(d);

    /* ---- block descriptor: split it ---- */
    if (level == 3)                         /* invalid encoding at L0 */
        return NULL;

    u64 *t = alloc_table();
    if (!t) return NULL;

    u64 base  = d & PTE_ADDR_MASK;
    u64 attrs = d & PTE_ATTR_MASK;
    u64 step  = (level == 2) ? PMD_SIZE : PAGE_SIZE;
    u64 type  = (level == 2) ? 0 : PTE_PAGE;       /* PMD blocks / PTE pages */

    for (u32 i = 0; i < PTRS_PER_PAGE; i++)
        t[i] = (base + (u64)i * step) | attrs | type | PTE_VALID;

    dsb(ishst);
    parent[idx] = 0;                        /* break */
    flush_tlb_local_all();
    parent[idx] = table_desc(t);            /* make  */
    dsb(ishst);
    return t;
}

/* Map one 4 KiB page. replace=false keeps an existing valid mapping. */
static int map_page(u64 *pgd, u64 va, u64 pa, u64 attrs, bool replace)
{
    u64 *pud = next_table(pgd, PGD_INDEX(va), 3);
    if (!pud) return MX_ENOMEM;
    u64 *pmd = next_table(pud, PUD_INDEX(va), 2);
    if (!pmd) return MX_ENOMEM;
    u64 *pte = next_table(pmd, PMD_INDEX(va), 1);
    if (!pte) return MX_ENOMEM;

    u64 idx = PTE_INDEX(va);
    if (!replace && (pte[idx] & PTE_VALID))
        return MX_OK;

    pte[idx] = (pa & PTE_ADDR_MASK) | attrs | PTE_PAGE | PTE_VALID;
    return MX_OK;
}

/* Map a 2 MiB block at PMD level if the slot is empty; returns true if done */
static bool map_block_2m(u64 *pgd, u64 va, u64 pa, u64 attrs)
{
    u64 *pud = next_table(pgd, PGD_INDEX(va), 3);
    if (!pud) return false;
    u64 *pmd = next_table(pud, PUD_INDEX(va), 2);
    if (!pmd) return false;
    u64 idx = PMD_INDEX(va);
    if (pmd[idx] & PTE_VALID)
        return false;                       /* something is already there */
    pmd[idx] = (pa & PTE_ADDR_MASK) | attrs | PTE_VALID;   /* block: bit1=0 */
    return true;
}

static u64 attrs_for(int attr)
{
    switch (attr) {
    case MMU_ATTR_RAM_RWX: return ATTRS_RWX;
    case MMU_ATTR_DEVICE:  return ATTRS_DEVICE;
    case MMU_ATTR_RAM_RW:
    default:               return ATTRS_RW_NX;
    }
}

/* -------------------------------------------------------
 * mmu_map_linear - map [pa, pa+size) at KERNEL_VA_BASE + pa using
 * 2 MiB blocks wherever possible (a 4 KiB-only linear map of a few GiB
 * of RAM would need thousands of tables). Existing mappings (e.g. the
 * kernel image) are never overwritten.
 * -------------------------------------------------------*/
int mmu_map_linear(phys_addr_t pa, size_t size, int attr)
{
    u64 a   = attrs_for(attr);
    u64 beg = pa & PAGE_MASK;
    u64 end = ALIGN(pa + size, PAGE_SIZE);
    int ret = MX_OK;

    u64 flags;
    spin_lock_irqsave(&g_pgd_lock, flags);
    while (beg < end) {
        u64 va = KERNEL_VA_BASE + beg;
        if (IS_ALIGNED(beg, PMD_SIZE) && end - beg >= PMD_SIZE &&
            map_block_2m(g_pgd_kernel, va, beg, a)) {
            beg += PMD_SIZE;
            continue;
        }
        ret = map_page(g_pgd_kernel, va, beg, a, false);
        if (ret) break;
        beg += PAGE_SIZE;
    }
    spin_unlock_irqrestore(&g_pgd_lock, flags);
    dsb(ishst);
    return ret;
}

/* -------------------------------------------------------
 * mmu_map_range - map a contiguous physical range, page granular,
 * replacing whatever is there (blocks are split as needed).
 * flags are full PTE attribute bits (type bits are added here; callers may
 * still pass the legacy PTE_VALID|PTE_PAGE bits - they are masked).
 * -------------------------------------------------------*/
int mmu_map_range(u64 va, u64 pa, size_t size, u64 flags)
{
    if (!IS_ALIGNED(va, PAGE_SIZE) || !IS_ALIGNED(pa, PAGE_SIZE))
        return MX_EINVAL;

    u64 attrs = flags & PTE_ATTR_MASK;
    int ret = MX_OK;
    u64 irqf;

    spin_lock_irqsave(&g_pgd_lock, irqf);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        ret = map_page(g_pgd_kernel, va + off, pa + off, attrs, true);
        if (ret) {
            /* roll back what this call already mapped */
            for (size_t o = 0; o < off; o += PAGE_SIZE) {
                u64 *pud = next_table(g_pgd_kernel, PGD_INDEX(va + o), 3);
                u64 *pmd = pud ? next_table(pud, PUD_INDEX(va + o), 2) : NULL;
                u64 *pte = pmd ? next_table(pmd, PMD_INDEX(va + o), 1) : NULL;
                if (pte) pte[PTE_INDEX(va + o)] = 0;
            }
            break;
        }
    }
    spin_unlock_irqrestore(&g_pgd_lock, irqf);
    dsb(ishst);
    if (g_mmu_on)
        mmu_tlb_flush_all();
    return ret;
}

/* -------------------------------------------------------
 * mmu_unmap_range - invalidate PTEs for [va, va+size).
 * Intermediate tables are kept (they are reused by the next mapping that
 * falls in the same 2 MiB window).
 * -------------------------------------------------------*/
int mmu_unmap_range(u64 va, size_t size)
{
    if (!IS_ALIGNED(va, PAGE_SIZE))
        return MX_EINVAL;

    u64 irqf;
    spin_lock_irqsave(&g_pgd_lock, irqf);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        u64 a = va + off;
        u64 d = g_pgd_kernel[PGD_INDEX(a)];
        if ((d & 3) != 3) continue;
        u64 *pud = table_from_desc(d);
        d = pud[PUD_INDEX(a)];
        if ((d & 3) != 3) continue;           /* block or empty: not ours */
        u64 *pmd = table_from_desc(d);
        d = pmd[PMD_INDEX(a)];
        if ((d & 3) != 3) continue;
        u64 *pte = table_from_desc(d);
        pte[PTE_INDEX(a)] = 0;
    }
    spin_unlock_irqrestore(&g_pgd_lock, irqf);
    dsb(ishst);
    if (g_mmu_on)
        mmu_tlb_flush_all();
    return MX_OK;
}

/* -------------------------------------------------------
 * mmu_early_init - called before MMU is turned on
 *   phys_base: physical load address of kernel
 *   virt_base: link-time VA of the kernel image
 *   kernel_sz: bytes of the image (incl. BSS)
 * Maps the image RWX (tightened later by mmu_protect_kernel).
 * -------------------------------------------------------*/
void mmu_early_init(phys_addr_t phys_base, u64 virt_base, size_t kernel_sz)
{
    g_kphys = phys_base;
    g_kvirt = virt_base;

    memset(g_pgd_kernel, 0, TABLE_SIZE);

    u64 aligned_sz = ALIGN(kernel_sz, PAGE_SIZE);
    g_ksize = (size_t)aligned_sz;

    /* kernel image at its link address ... */
    for (size_t off = 0; off < aligned_sz; off += PAGE_SIZE) {
        if (map_page(g_pgd_kernel, virt_base + off, phys_base + off,
                     ATTRS_RWX, true) != MX_OK)
            monox_panic("MMU: out of page tables mapping the kernel image\n");
    }
    /* ... and at the identity (low) alias the boot CPU is executing from
     * (separate entries: the high alias no longer coincides with the
     * linear map). */
    for (size_t off = 0; off < aligned_sz; off += PAGE_SIZE) {
        if (map_page(g_pgd_kernel, phys_base + off, phys_base + off,
                     ATTRS_RWX, false) != MX_OK)
            monox_panic("MMU: out of page tables mapping the kernel image\n");
    }

    pr_info("MMU: kernel mapped [0x%llx .. 0x%llx) -> [0x%llx)\n",
            virt_base, virt_base + aligned_sz, phys_base);
}

/* -------------------------------------------------------
 * mmu_enable - turn on MMU (call after mmu_early_init)
 * -------------------------------------------------------*/
void mmu_enable(void)
{
    /* ---- derive TCR.IPS / TCR.AS from what this CPU implements ---- */
    u64 mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    u64 ips = mmfr0 & 0xF;                  /* PARange */
    if (ips > 5) ips = 5;                   /* we use at most 48-bit PA */
    u64 tcr = MONOX_TCR_BASE | (ips << 32);
    if (((mmfr0 >> 4) & 0xF) == 2)          /* ASIDBits == 16 */
        tcr |= TCR_AS_16BIT;
    tcr |= TCR_HA | TCR_HD;                 /* RES0 if FEAT_HAFDBS is absent */

    u64 mair = MONOX_MAIR_VALUE;
    u64 ttbr = virt_to_phys_any(g_pgd_kernel);

    /* The tables were written with caches off: push them to memory so the
     * table walker (cacheable) cannot see stale lines. */
    cache_flush_range(g_pgd_kernel, TABLE_SIZE);
    cache_flush_range(g_early_pt_pool, g_early_pt_used);
    __asm__ volatile("ic iallu" ::: "memory");
    dsb(nsh);
    isb();

    __asm__ volatile("msr mair_el1, %0" :: "r"(mair) : "memory");
    __asm__ volatile("msr tcr_el1, %0"  :: "r"(tcr)  : "memory");
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr) : "memory");
    __asm__ volatile("msr ttbr1_el1, %0" :: "r"(ttbr) : "memory");
    isb();

    __asm__ volatile("tlbi vmalle1" ::: "memory");
    dsb(nsh);
    isb();

    u64 sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12); /* M + C + I */
    __asm__ volatile("msr sctlr_el1, %0" :: "r"(sctlr) : "memory");
    isb();

    g_mmu_on = true;
    g_kernel_locks_enabled = 1;             /* exclusives are legal now */
    dsb(ish);

    /* Publish the register values for secondary CPUs (head.S). They run
     * with caches off, so the data must reach memory. */
    secondary_mmu_regs[0] = mair;
    secondary_mmu_regs[1] = tcr;
    secondary_mmu_regs[2] = ttbr;
    secondary_mmu_regs[3] = sctlr;
    cache_flush_range((void *)secondary_mmu_regs, 4 * sizeof(u64));

    pr_info("MMU: enabled (TCR=0x%llx MAIR=0x%llx)\n", tcr, mair);
}

/* -------------------------------------------------------
 * TLB maintenance
 * -------------------------------------------------------*/
void mmu_tlb_flush_all(void)
{
    dsb(ishst);
    __asm__ volatile("tlbi vmalle1is" ::: "memory");
    dsb(ish);
    isb();
}

void mmu_tlb_flush_page(u64 va)
{
    dsb(ishst);
    __asm__ volatile("tlbi vaae1is, %0" :: "r"(va >> 12) : "memory");
    dsb(ish);
    isb();
}

/* -------------------------------------------------------
 * mmu_map_device - map MMIO region (device memory, non-cacheable)
 * -------------------------------------------------------*/
int mmu_map_device(u64 va, u64 pa, size_t size)
{
    return mmu_map_range(va, pa, size, ATTRS_DEVICE);
}

/* -------------------------------------------------------
 * mmu_protect_kernel - tighten permissions after init
 *   text: rx, rodata: ro, data/bss: rw noexec
 * The VAs may be the low (identity) alias the boot CPU is running at;
 * virt_to_phys_any() copes with both.
 * -------------------------------------------------------*/
/* Apply 'flags' to one image range at BOTH aliases:
 *  - the high link-time alias the kernel executes from, and
 *  - the low alias (identity == linear map of the same pages).
 * (With the image no longer sitting inside the linear map these are
 *  different page-table entries; hardening only one would leave the other
 *  writable/executable.) */
static int protect_range(u64 va, size_t size, u64 flags)
{
    u64 pa = virt_to_phys_any((void *)va);
    u64 hi = g_kvirt + (pa - g_kphys);
    size = ALIGN(size, PAGE_SIZE);
    int r1 = mmu_map_range(hi, pa, size, flags);
    int r2 = mmu_map_range(pa, pa, size, flags);
    return r1 ? r1 : r2;
}

void mmu_protect_kernel(u64 text_va,   size_t text_sz,
                        u64 rodata_va, size_t rodata_sz,
                        u64 data_va,   size_t data_sz)
{
    u64 f_text = PTE_AF|PTE_SH_INNER|PTE_ATTR(ATTR_NORMAL)|PTE_UXN|PTE_RO;
    u64 f_ro   = PTE_AF|PTE_SH_INNER|PTE_ATTR(ATTR_NORMAL)|PTE_UXN|PTE_PXN|PTE_RO;
    u64 f_rw   = PTE_AF|PTE_SH_INNER|PTE_ATTR(ATTR_NORMAL)|PTE_UXN|PTE_PXN;

    int r1 = protect_range(text_va,   text_sz,   f_text);
    int r2 = protect_range(rodata_va, rodata_sz, f_ro);
    int r3 = protect_range(data_va,   data_sz,   f_rw);
    mmu_tlb_flush_all();
    __asm__ volatile("ic iallu" ::: "memory");
    dsb(ish);
    isb();

    if (r1 || r2 || r3)
        pr_err("MMU: hardening incomplete (text=%d rodata=%d data=%d)\n", r1, r2, r3);
    else
        pr_info("MMU: kernel permissions hardened\n");
}
