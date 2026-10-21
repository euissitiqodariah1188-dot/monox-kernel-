/* MONOX Kernel - Main Entry Point (C)
 *
 * Called from arch/arm64/boot/head.S after:
 *   - BSS zeroed
 *   - Early stack set up
 *   - VBAR set
 *   - CPU features detected
 *   - Early UART ready
 *
 * x0 = physical address of DTB
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "../arch/arm64/cpu/cpu.h"
#include "../arch/arm64/cpu/irq.h"
#include "../arch/arm64/mm/mmu.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../kernel/sched/sched.h"
#include "../fs/vfs/vfs.h"
#include "../security/satpam/traffic_satpam.h"
#include "../security/syscall_filter/system_satpam.h"
#include "../net/wifi/wifi.h"
#include "../drivers/gpu/gpu_driver.h"
#include "../drivers/usb/usb.h"
#include "../io/touchscreen/touchscreen.h"
#include "dtb.h"

/* -------------------------------------------------------
 * UART physical base (written by early_uart_init)
 * -------------------------------------------------------*/
phys_addr_t g_uart_phys_base = 0;

/* -------------------------------------------------------
 * Secondary CPU entry (SMP boot)
 * -------------------------------------------------------*/
static volatile u32 g_cpu_online_flag[MONOX_MAX_CPUS];

void secondary_start(void)
{
    u32 cpu = smp_processor_id();
    cpu_detect_features();
    cpu_configure_el1();
    gic_cpu_init();
    sched_cpu_init(cpu);

    pr_info("CPU%u: online\n", cpu);
    g_cpu_online_flag[cpu] = 1;
    dsb(ish);
    local_irq_enable();

    /* Drop into idle loop */
    for (;;) {
        schedule();
        cpu_wfi();
    }
}

/* wait up to 'ms' milliseconds using the generic counter (not a spin count) */
static void wait_ms(u32 ms)
{
    u64 freq, start, now;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq) freq = 1000000;                   /* firmware did not set it */
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
    u64 ticks = freq / 1000 * ms;
    do {
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
        cpu_relax();
    } while (now - start < ticks);
}

/* -------------------------------------------------------
 * Boot all secondary CPUs
 *
 * The per-CPU stack is released again when the CPU does not report in:
 * the old code allocated 16 KiB per CPU unconditionally and leaked it
 * (nothing ever started those CPUs).
 * -------------------------------------------------------*/
static void smp_boot_secondaries(u32 nr_cpus)
{
    extern volatile u64 secondary_spin_table[];

    for (u32 cpu = 1; cpu < nr_cpus && cpu < MONOX_MAX_CPUS; cpu++) {
        phys_addr_t stack_pa = pmm_alloc_pages(2, GFP_KERNEL);
        if (!stack_pa) { pr_warn("SMP: no stack for CPU%u\n", cpu); continue; }

        u64 stack_top = (u64)phys_to_virt_pmm(stack_pa) + (4 * 4096);

        g_cpu_online_flag[cpu] = 0;
        WRITE_ONCE(secondary_spin_table[cpu], stack_top);
        dsb(ish);
        __asm__ volatile("sev" ::: "memory");

        /* up to 100 ms for the CPU to come online */
        for (u32 waited = 0; waited < 100 && !g_cpu_online_flag[cpu]; waited++)
            wait_ms(1);

        if (g_cpu_online_flag[cpu]) {
            pr_info("SMP: CPU%u is online\n", cpu);
        } else {
            /* never started: take the slot back and free the stack */
            WRITE_ONCE(secondary_spin_table[cpu], 0);
            dsb(ish);
            pmm_free_pages(stack_pa, 2);
            pr_warn("SMP: CPU%u did not come online (no PSCI CPU_ON issued)\n", cpu);
        }
    }
}

/* -------------------------------------------------------
 * Block device lookup stub (used by MonoxFS mount)
 * -------------------------------------------------------*/
struct monoxfs_priv;
int blk_find_dev(const char *name,
                 int (**bread)(struct monoxfs_priv *, u64, void *),
                 int (**bwrite)(struct monoxfs_priv *, u64, const void *),
                 void **handle)
{
    /* Real implementation would search NVMe/eMMC device list */
    (void)name; (void)handle;
    *bread  = NULL;
    *bwrite = NULL;
    pr_warn("BLK: block device '%s' not yet registered\n", name);
    return MX_ENODEV;
}

/* -------------------------------------------------------
 * Service dispatch stub (used by init system)
 * -------------------------------------------------------*/
int service_dispatch(const char *exec_start)
{
    pr_info("SERVICE: would exec '%s'\n", exec_start);
    return MX_OK;
}

/* -------------------------------------------------------
 * GPIO stub (used by touchscreen reset)
 * -------------------------------------------------------*/
void gpio_set(u32 gpio, bool val)
{
    (void)gpio; (void)val;
}

void udelay(u32 us)
{
    u64 freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    u64 ticks = (u64)us * freq / 1000000ULL;
    u64 start;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(start));
    while (1) {
        u64 now;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(now));
        if (now - start >= ticks) break;
    }
}

/* -------------------------------------------------------
 * One-shot timers on the ARM generic *virtual* timer (PPI 27)
 *
 * The virtual timer is used because CNTVCT/CNTV_* are always accessible from
 * EL1, whatever EL2 did. It used to be a stub that invoked the callback
 * immediately, which turned task_sleep() into a plain yield (the WiFi
 * auto-connect task then rescanned in a busy loop instead of every 30 s).
 * Without a working GIC the old immediate-callback behaviour is kept, so a
 * sleeping task can never wait for an interrupt that will not come.
 * -------------------------------------------------------*/
#define KTIMER_IRQ        27          /* virtual timer PPI */
#define KTIMER_MAX        64

struct ktimer {
    u64   deadline;                   /* counter ticks */
    void (*cb)(void *);
    void *arg;
    bool  used;
};

static struct ktimer g_ktimers[KTIMER_MAX];
static spinlock_t    g_ktimer_lock = SPIN_LOCK_INIT;
static bool          g_ktimer_ready = false;
static u64           g_ktimer_freq  = 0;

static __always_inline u64 cnt_now(void)
{
    u64 v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* program the compare register for the earliest pending timer (lock held) */
static void ktimer_reprogram(void)
{
    u64 best = 0; bool any = false;
    for (u32 i = 0; i < KTIMER_MAX; i++) {
        if (g_ktimers[i].used && (!any || g_ktimers[i].deadline < best)) {
            best = g_ktimers[i].deadline; any = true;
        }
    }
    if (any) {
        __asm__ volatile("msr cntv_cval_el0, %0" :: "r"(best));
        __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(1ULL));   /* ENABLE, unmasked */
    } else {
        __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(0ULL));   /* off */
    }
    isb();
}

static void ktimer_irq(u32 irq, void *data)
{
    (void)irq; (void)data;

    for (;;) {
        void (*cb)(void *) = NULL;
        void *arg = NULL;

        spin_lock(&g_ktimer_lock);
        u64 now = cnt_now();
        for (u32 i = 0; i < KTIMER_MAX; i++) {
            if (g_ktimers[i].used && g_ktimers[i].deadline <= now) {
                cb  = g_ktimers[i].cb;
                arg = g_ktimers[i].arg;
                g_ktimers[i].used = false;
                break;
            }
        }
        if (!cb) {
            ktimer_reprogram();           /* also silences the line */
            spin_unlock(&g_ktimer_lock);
            return;
        }
        spin_unlock(&g_ktimer_lock);
        cb(arg);                          /* outside the lock: may call task_wake */
    }
}

static void ktimer_init(bool gic_ok)
{
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(g_ktimer_freq));
    if (!g_ktimer_freq) {
        g_ktimer_freq = 24000000;         /* firmware left CNTFRQ unset */
        pr_warn("TIMER: CNTFRQ is 0, assuming 24 MHz\n");
    }
    __asm__ volatile("msr cntv_ctl_el0, %0" :: "r"(0ULL));
    spin_lock_init(&g_ktimer_lock);
    memset(g_ktimers, 0, sizeof(g_ktimers));

    if (!gic_ok || !gic_is_ready()) {
        pr_warn("TIMER: no working GIC, one-shot timers fall back to immediate callbacks\n");
        return;
    }
    if (irq_register(KTIMER_IRQ, ktimer_irq, NULL) != MX_OK) {
        pr_warn("TIMER: cannot register IRQ %u\n", KTIMER_IRQ);
        return;
    }
    g_ktimer_ready = true;
    pr_info("TIMER: generic virtual timer ready (%llu Hz)\n", g_ktimer_freq);
}

void timer_oneshot(u64 ns, void (*cb)(void *), void *arg)
{
    if (!cb) return;

    if (!g_ktimer_ready) {                /* no interrupts: behave like before */
        cb(arg);
        return;
    }

    u64 ticks = (ns / 1000000000ULL) * g_ktimer_freq +
                (ns % 1000000000ULL) * g_ktimer_freq / 1000000000ULL;

    u64 flags;
    spin_lock_irqsave(&g_ktimer_lock, flags);
    struct ktimer *slot = NULL;
    for (u32 i = 0; i < KTIMER_MAX; i++) {
        if (!g_ktimers[i].used) { slot = &g_ktimers[i]; break; }
    }
    if (slot) {
        slot->deadline = cnt_now() + ticks;
        slot->cb   = cb;
        slot->arg  = arg;
        slot->used = true;
        ktimer_reprogram();
    }
    spin_unlock_irqrestore(&g_ktimer_lock, flags);

    if (!slot) {
        /* table full: fire now rather than leave a task asleep forever */
        pr_warn("TIMER: timer table full, waking immediately\n");
        cb(arg);
    }
}

/* -------------------------------------------------------
 * helpers for monox_start
 * -------------------------------------------------------*/
static bool range_overlaps(u64 a0, u64 a1, u64 b0, u64 b1)
{
    return a0 < b1 && b0 < a1;
}

/* Map every RAM region into the linear map, leaving out reserved-memory
 * carve-outs (firmware/TrustZone memory must never be mapped cacheable:
 * speculative accesses to it can raise SErrors). */
static void map_ram_linear(const struct board_info *b)
{
    for (u32 i = 0; i < b->nr_regions; i++) {
        if (b->regions[i].type != MEM_TYPE_RAM || !b->regions[i].size)
            continue;

        u64 cur = b->regions[i].base;
        u64 end = cur + b->regions[i].size;

        while (cur < end) {
            /* earliest reserved range intersecting [cur,end) */
            u64 rb = end, re = end;
            bool found = false;
            for (u32 j = 0; j < b->nr_regions; j++) {
                if (b->regions[j].type != MEM_TYPE_RESERVED) continue;
                u64 r0 = b->regions[j].base;
                u64 r1 = r0 + b->regions[j].size;
                if (!range_overlaps(cur, end, r0, r1)) continue;
                u64 start = r0 > cur ? r0 : cur;
                if (!found || start < rb) { rb = start; re = r1; found = true; }
            }
            if (!found) {
                if (mmu_map_linear(cur, (size_t)(end - cur), MMU_ATTR_RAM_RW))
                    monox_panic("MMU: cannot map RAM 0x%llx..0x%llx\n", cur, end);
                break;
            }
            if (rb > cur &&
                mmu_map_linear(cur, (size_t)(rb - cur), MMU_ATTR_RAM_RW))
                monox_panic("MMU: cannot map RAM 0x%llx..0x%llx\n", cur, rb);
            cur = re;
        }
    }
}

/* -------------------------------------------------------
 * Boot flow
 *
 *   head.S -> monox_start()       runs at the PHYSICAL (low) alias, MMU off
 *             parse DTB, console, page tables, MMU on
 *          -> __enter_virt()      jump to the HIGH link address
 *          -> monox_start_virt()  runs at KERNEL_IMAGE_VMA: everything else
 *
 * Rules for the pre-MMU part (monox_start .. mmu_enable):
 *   - no pointer tables / ops tables / string tables: a statically
 *     initialised pointer holds a HIGH address that is not mapped yet;
 *   - no exclusive (LDXR/STXR) instructions (locks are no-ops until the
 *     MMU is on, see g_kernel_locks_enabled);
 *   - no unaligned accesses (-mstrict-align).
 * -------------------------------------------------------*/

/* Optional raw progress markers: `make EARLY_CON=0x09000000` (QEMU virt)
 * prints M / V on the PL011 without any driver, to find out how far a silent
 * boot got. 1/2/3 come from head.S. */
#ifdef EARLY_CON_BASE
#define DBG_MARK(c) do { *(volatile u8 *)(u64)(EARLY_CON_BASE) = (u8)(c); } while (0)
#else
#define DBG_MARK(c) do { } while (0)
#endif

extern void __noreturn __enter_virt(phys_addr_t dtb_phys, u64 sp_delta);
extern void __noreturn monox_start_virt(phys_addr_t dtb_phys);
extern char monox_vectors[];

/* Static: 128 regions + strings are too big for the 16 KiB boot stack, and
 * both halves of the boot flow share it. BSS is zeroed by head.S. */
static struct board_info g_board;
static phys_addr_t       g_kphys_base;
static size_t            g_ksize_bytes;

void __noreturn monox_start(phys_addr_t dtb_phys)
{
    struct board_info *board = &g_board;

#ifdef EARLY_CON_BASE
    /* Console up before the DTB is even parsed: nothing is lost if the
     * parser itself hangs. */
    g_uart_phys_base = EARLY_CON_BASE;
    early_uart_init();
#endif

    /* 1. Parse DTB first - it tells us where the UART is. The MMU is still
     *    off, so anything printed before the UART is known is kept in the
     *    log buffer and replayed by early_uart_init(). */
    pr_info("\n");
    pr_info("MONOX KERNEL v%u.%u.%u '%s' - ARM64 (ARMv8 -> ARMv9)\n",
            MONOX_VERSION_MAJOR, MONOX_VERSION_MINOR,
            MONOX_VERSION_PATCH, MONOX_CODENAME);

    memset(board, 0, sizeof(*board));
    dtb_parse(dtb_phys, board);

    /* The old code stored the address but never initialised the UART
     * again after head.S had run it with a zero address => no output. */
    if (board->uart_base)
        g_uart_phys_base = board->uart_base;
    early_uart_init();

    pr_info("Board: %s\n", board->model[0] ? board->model : "unknown");
    pr_info("RAM:   %llu MiB\n", board->total_ram / (1024 * 1024));

    if (!board->total_ram)
        monox_panic("No usable RAM found in the DTB (dtb phys=0x%llx)\n", dtb_phys);

    cpu_set_psci_conduit(board->psci_hvc);

    /* 2. CPU features */
    cpu_detect_features();
    cpu_print_info(0);

    /* 3. Where are we? (PC-relative symbol addresses = physical while the
     *    MMU is off). */
    extern char __kernel_start[], __kernel_end[];
    extern u64  __kernel_phys_base;
    phys_addr_t kphys = __kernel_phys_base ? __kernel_phys_base
                                           : (phys_addr_t)(u64)__kernel_start;
    /* __kernel_start / __kernel_end are PC-relative here, so their
     * difference is the image size even though both are physical */
    size_t ksz = (size_t)((u64)__kernel_end - (u64)__kernel_start);
    g_kphys_base  = kphys;
    g_ksize_bytes = ksz;
    board->kernel_phys_base = kphys;
    board->kernel_size      = ksz;

    pr_info("Image: phys=0x%llx size=%zu KiB (linked at 0x%llx)\n",
            kphys, ksz / 1024, (u64)KERNEL_IMAGE_VMA);

    /* 4. Reservations are registered BEFORE the allocator exists so that
     *    nothing inside them is ever written. */
    pmm_reserve_range(kphys, ksz);                                   /* image   */
    if (board->dtb_size)
        pmm_reserve_range(board->dtb_phys, board->dtb_size);         /* DTB     */
    for (u32 i = 0; i < board->nr_regions; i++)
        if (board->regions[i].type == MEM_TYPE_RESERVED)
            pmm_reserve_range(board->regions[i].base, board->regions[i].size);

    /* 5. MMU: kernel image (high alias + identity) + linear map of RAM + the
     *    MMIO windows we need right after the switch (UART for logging,
     *    GIC). Everything is mapped BEFORE the MMU is enabled. */
    mmu_early_init(kphys, KERNEL_IMAGE_VMA, ksz);
    map_ram_linear(board);

    if (board->uart_base)
        mmu_map_device(KERNEL_VA_BASE + board->uart_base, board->uart_base, 0x1000);
#ifdef EARLY_CON_BASE
    if (board->uart_base != EARLY_CON_BASE)
        mmu_map_device(KERNEL_VA_BASE + EARLY_CON_BASE, EARLY_CON_BASE, 0x1000);
#endif
    if (board->gic_dist_base)
        mmu_map_device(KERNEL_VA_BASE + board->gic_dist_base, board->gic_dist_base, 0x10000);
    if (board->gic_redist_base) {
        u64 stride = board->gic_redist_stride ? board->gic_redist_stride : 0x20000;
        mmu_map_device(KERNEL_VA_BASE + board->gic_redist_base, board->gic_redist_base,
                       (size_t)(stride * MONOX_MAX_CPUS));
    }

    mmu_enable();
    DBG_MARK('M');                         /* MMU is on, still at the low alias */

    /* 6. Continue at the link-time (high) address. Never returns. */
    __enter_virt(dtb_phys, mmu_virt_delta());
}

/* -------------------------------------------------------
 * monox_start_virt - runs at KERNEL_IMAGE_VMA with the MMU on
 * -------------------------------------------------------*/
void __noreturn monox_start_virt(phys_addr_t dtb_phys)
{
    (void)dtb_phys;
    struct board_info *board = &g_board;
    phys_addr_t kphys = g_kphys_base;
    (void)kphys;

    /* Exceptions must be taken at the high alias too: handlers running at
     * the low alias would compute LOW addresses for statics. */
    __asm__ volatile("msr vbar_el1, %0" :: "r"((u64)monox_vectors) : "memory");
    isb();
    DBG_MARK('V');                         /* now executing at the high alias */
    pr_info("MONOX: running at virtual address 0x%llx\n", (u64)(uintptr_t)&monox_start_virt);

    /* 7. Physical memory manager */
    pmm_init(board->regions, board->nr_regions, board->total_ram);
    pmm_stats();

    /* 8. Virtual memory manager + slab allocator */
    vmm_init();
    kmalloc_init();
    pr_info("VMM:   kmalloc ready\n");

    /* 9. GIC interrupt controller */
    bool gic_ok = false;
    if (board->gic_dist_base && board->gic_redist_base) {
        if (gic_init(board->gic_dist_base, board->gic_redist_base,
                     board->gic_redist_stride) == MX_OK)
            gic_ok = (gic_cpu_init() == MX_OK);
        if (!gic_ok)
            pr_err("GIC:   init failed, interrupts unavailable\n");
    } else {
        pr_warn("GIC:   no GICv3 in the DTB (QEMU: use -M virt,gic-version=3), interrupts unavailable\n");
    }
    cpu_configure_el1();
    ktimer_init(gic_ok);
    local_irq_enable();
    pr_info("GIC:   interrupts enabled\n");

    /* 10. Security: traffic satpam + system satpam */
    satpam_init();
    sysatpam_init();

    /* Protect kernel text */
    extern char __text_start[], __text_end[];
    sysatpam_protect_region((u64)__text_start, (u64)__text_end,
                             "kernel_text", false);
    pr_info("SATPAM: kernel protection active\n");

    /* 11. Scheduler */
    sched_init();
    sched_cpu_init(0);

    /* 12. VFS */
    vfs_init();

    /* 13. GPU */
    if (board->gpu_mmio) {
        gpu_init(board->gpu_mmio, board->gpu_mmio_size,
                 board->gpu_irq, board->gpu_compatible);
    }

    /* 14. USB */
    if (board->xhci_mmio) {
        usb_init(board->xhci_mmio, board->xhci_mmio_size, board->xhci_irq);
    }

    /* 15. WiFi */
    wifi_init();

    /* 16. Touchscreen (if DTB has touch panel) */
    if (board->ts_dtb_props[0]) {
        ts_probe((const char **)board->ts_dtb_props);
    }

    /* 17. SMP: boot secondary CPUs */
    u32 nr_cpus = cpu_count();
    if (nr_cpus > 1) {
        smp_boot_secondaries(nr_cpus);
    }

    /* 18. Tighten kernel memory permissions. Data covers .data AND .bss. */
    extern char __rodata_start[], __rodata_end[], __data_start[], __bss_end[];
    mmu_protect_kernel(
        (u64)__text_start,   (size_t)((u64)__text_end   - (u64)__text_start),
        (u64)__rodata_start, (size_t)((u64)__rodata_end - (u64)__rodata_start),
        (u64)__data_start,   (size_t)((u64)__bss_end    - (u64)__data_start)
    );

    pr_info("MONOX: all subsystems ready, starting init (PID 1)\n");

    /* 19. Launch PID 1 (init system) as a kernel task */
    extern void __noreturn monox_init(void);
    task_create("monox_init", (void (*)(void *))monox_init, NULL,
                PRIO_NORMAL, 65536);

    /* 20. Enable local IRQs and drop to scheduler idle loop */
    local_irq_enable();

    for (;;) {
        schedule();
        cpu_wfi();
    }
}
