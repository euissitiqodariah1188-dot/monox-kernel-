/* Host test for arch/arm64/mm/mmu.c: builds the real page tables in host memory
 * (the "physical" addresses of the tables are host pointers, which is exactly
 * what mmu.c assumes while the MMU is off) and checks them with an independent
 * software walker that follows the ARMv8 4 KiB-granule descriptor format. */
#include "arch/arm64/mm/mmu.c"          /* the unit under test, statics included */
int printf(const char *, ...); void exit(int);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

phys_addr_t pmm_alloc_pages(u32 o, gfp_t f) { return 0; }
void monox_panic(const char *f, ...) { printf("PANIC\n"); exit(9); }
void monox_printk(const char *l, const char *f, ...) {}
void local_irq_disable(void) {} u64 local_irq_save(void) { return 0; } void local_irq_restore(u64 f) {}
volatile u64 secondary_mmu_regs[4]; u64 __kernel_phys_base;

/* independent walker ----------------------------------------------------- */
struct tr { bool ok; u64 pa; u64 desc; int level; };
static struct tr walk(u64 va)
{
    struct tr r = { false, 0, 0, 0 };
    /* TTBR0/TTBR1 range check: top 16 bits must be all 0 or all 1 */
    u64 top = va >> 48;
    if (top != 0 && top != 0xFFFF) return r;
    u64 *t = g_pgd_kernel;
    for (int lvl = 0; lvl < 4; lvl++) {
        int shift = 39 - 9 * lvl;
        u64 d = t[(va >> shift) & 0x1FF];
        if (!(d & 1)) return r;
        r.desc = d; r.level = lvl;
        bool table = (d & 2) && lvl < 3;
        bool page  = (d & 2) && lvl == 3;
        if (lvl == 0 && !table) return r;              /* no L0 blocks */
        if (table) { t = (u64 *)(d & 0x0000FFFFFFFFF000ULL); continue; }
        if (lvl < 3 && (d & 2)) return r;
        if (lvl == 3 && !page) return r;
        u64 mask = (1ULL << shift) - 1;
        r.pa = (d & 0x0000FFFFFFFFF000ULL & ~mask) | (va & mask);
        r.ok = true; return r;
    }
    return r;
}

#define MiB (1024ULL * 1024)
int main(void)
{
    /* QEMU virt: image loaded at RAM base + 2 MiB, ~2.5 MiB incl. BSS */
    const u64 KPHYS = 0x40200000ULL, KSZ = 2560 * 1024ULL;
    mmu_early_init(KPHYS, KERNEL_IMAGE_VMA, KSZ);

    /* image: high alias AND identity alias */
    for (u64 off = 0; off < KSZ; off += 4096) {
        struct tr h = walk(KERNEL_IMAGE_VMA + off), l = walk(KPHYS + off);
        CHECK(h.ok && h.pa == KPHYS + off && h.level == 3);
        CHECK(l.ok && l.pa == KPHYS + off);
    }
    CHECK(!walk(KERNEL_IMAGE_VMA + KSZ).ok);                    /* nothing beyond the image */
    CHECK(!walk(KERNEL_IMAGE_VMA - 4096).ok);

    /* the image window must not collide with the linear map */
    CHECK(KERNEL_IMAGE_VMA >= KERNEL_VA_BASE + (1ULL << 40));

    /* linear map of 1 GiB RAM at 0x40000000 (image already present inside it) */
    CHECK(mmu_map_linear(0x40000000ULL, 1024 * MiB, MMU_ATTR_RAM_RW) == MX_OK);
    u64 samples[] = { 0x40000000ULL, 0x401FF000ULL, 0x40200000ULL, 0x40500000ULL,
                      0x40600000ULL, 0x50000000ULL, 0x7FFFF000ULL, 0x7FE00000ULL };
    for (unsigned i = 0; i < sizeof samples / sizeof *samples; i++) {
        u64 pa = samples[i];
        struct tr a = walk(KERNEL_VA_BASE + pa), b = walk(pa);
        CHECK(a.ok && a.pa == pa); CHECK(b.ok && b.pa == pa);
    }
    CHECK(walk(KERNEL_VA_BASE + 0x40000000ULL).level == 2);     /* 2 MiB block, not 512 PTEs */
    CHECK(walk(KERNEL_VA_BASE + 0x60000000ULL).level == 2);
    CHECK(walk(KERNEL_VA_BASE + 0x40200000ULL).level == 3);     /* image pages untouched */
    CHECK(!walk(KERNEL_VA_BASE + 0x80000000ULL).ok);            /* past the end of RAM */
    CHECK(!walk(KERNEL_VA_BASE + 0x3FFFF000ULL).ok);            /* before RAM */
    CHECK((walk(KERNEL_VA_BASE + 0x60000000ULL).desc >> 53 & 3) == 3);   /* PXN|UXN on RAM */

    /* MMIO */
    CHECK(mmu_map_device(KERNEL_VA_BASE + 0x9000000ULL, 0x9000000ULL, 0x1000) == MX_OK);
    struct tr u = walk(0x9000000ULL);
    CHECK(u.ok && u.pa == 0x9000000ULL && ((u.desc >> 2) & 7) == 0);     /* Device-nGnRnE */

    /* mapping a single 4K page inside an existing 2 MiB block must SPLIT it
     * (the old walker treated the block descriptor as a table pointer) */
    u64 va = KERNEL_VA_BASE + 0x61000000ULL;
    CHECK(walk(va).level == 2);
    CHECK(mmu_map_range(va + 0x3000, 0x1234000ULL, 4096, ATTRS_RW_NX) == MX_OK);
    CHECK(walk(va + 0x3000).pa == 0x1234000ULL);
    CHECK(walk(va + 0x2000).pa == 0x61002000ULL && walk(va + 0x2000).level == 3);   /* neighbours preserved */
    CHECK(walk(va + 0x4000).pa == 0x61004000ULL);
    CHECK(walk(va - 0x1000).pa == 0x60FFF000ULL);                                    /* other block intact */

    /* vmm style range + unmap */
    u64 kv = 0xFFFF800000010000ULL;
    CHECK(mmu_map_range(kv, 0x70000000ULL, 16 * 4096, PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER | PTE_ATTR(ATTR_NORMAL)) == MX_OK);
    for (int i = 0; i < 16; i++) CHECK(walk(kv + i * 4096ULL).pa == 0x70000000ULL + i * 4096ULL);
    CHECK(!walk(kv + 16 * 4096ULL).ok);                                              /* guard page stays unmapped */
    CHECK(mmu_unmap_range(kv, 16 * 4096) == MX_OK);
    for (int i = 0; i < 16; i++) CHECK(!walk(kv + i * 4096ULL).ok);
    CHECK(mmu_map_range(kv, 0x70010000ULL, 4096, ATTRS_RW_NX) == MX_OK);             /* VA is reusable */
    CHECK(walk(kv).pa == 0x70010000ULL);

    /* unaligned / bad args */
    CHECK(mmu_map_range(kv + 1, 0, 4096, 0) == MX_EINVAL);

    /* address conversions */
    CHECK(virt_to_phys_any((void *)(KERNEL_IMAGE_VMA + 0x1234)) == KPHYS + 0x1234);
    CHECK(virt_to_phys_any((void *)(KERNEL_VA_BASE + 0x50000000ULL)) == 0x50000000ULL);
    CHECK(virt_to_phys_any((void *)0x40201000ULL) == 0x40201000ULL);
    CHECK(mmu_virt_delta() == KERNEL_IMAGE_VMA - KPHYS);

    /* hardening applies to BOTH aliases */
    mmu_protect_kernel(KERNEL_IMAGE_VMA + 0x1000, 0x8000,        /* text   */
                       KERNEL_IMAGE_VMA + 0x9000, 0x4000,        /* rodata */
                       KERNEL_IMAGE_VMA + 0xD000, 0x10000);      /* data+bss */
    u64 t_hi = walk(KERNEL_IMAGE_VMA + 0x1000).desc, t_lo = walk(KPHYS + 0x1000).desc;
    CHECK((t_hi & (1ULL << 7)) && (t_lo & (1ULL << 7)));                 /* RO on both */
    CHECK(!(t_hi & (1ULL << 53)) && !(t_lo & (1ULL << 53)));             /* text stays executable */
    u64 r_hi = walk(KERNEL_IMAGE_VMA + 0x9000).desc, r_lo = walk(KPHYS + 0x9000).desc;
    CHECK((r_hi & (1ULL << 7)) && (r_hi >> 53 & 1) && (r_lo >> 53 & 1));  /* rodata RO + NX */
    u64 d_hi = walk(KERNEL_IMAGE_VMA + 0xD000).desc, d_lo = walk(KPHYS + 0xD000).desc;
    CHECK(!(d_hi & (1ULL << 7)) && (d_hi >> 53 & 1) && !(d_lo & (1ULL << 7)) && (d_lo >> 53 & 1));
    CHECK(walk(KERNEL_IMAGE_VMA + 0x1000).pa == KPHYS + 0x1000);         /* still the same page */
    printf("ALL MMU TESTS PASSED (pool tables used: %zu of %d)\n", g_early_pt_used / 4096, EARLY_PT_POOL_TABLES);
    return 0;
}
