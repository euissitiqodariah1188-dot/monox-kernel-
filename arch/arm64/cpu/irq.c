/* MONOX Kernel - ARM64 IRQ / GICv3 Controller
 * Supports GICv3 and GICv4 (LPI)
 * Generic enough via DTB-parsed base addresses (no hardcoded addresses)
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "cpu.h"
#include "irq.h"

/* -------------------------------------------------------
 * GICv3 register layout (read from DTB at init)
 * -------------------------------------------------------*/
static struct gic_context {
    volatile u32 *gicd_base;    /* Distributor */
    volatile u64 *gicr_base;    /* Redistributor (per-CPU stride) */
    u32 nr_irqs;
    u32 nr_cpus;
    u32 gicr_stride;            /* bytes between Redistributors */
    spinlock_t lock;
    struct irq_desc *desc;      /* irq descriptor table (kmalloc'd) */
} g_gic;

/* -------------------------------------------------------
 * GICv3 Distributor offsets
 * -------------------------------------------------------*/
#define GICD_CTLR        0x0000
#define GICD_TYPER       0x0004
#define GICD_IIDR        0x0008
#define GICD_STATUSR     0x0010
#define GICD_IGROUPR(n)  (0x0080 + (n)*4)
#define GICD_ISENABLER(n)(0x0100 + (n)*4)
#define GICD_ICENABLER(n)(0x0180 + (n)*4)
#define GICD_ISPENDR(n)  (0x0200 + (n)*4)
#define GICD_ICPENDR(n)  (0x0280 + (n)*4)
#define GICD_ISACTIVER(n)(0x0300 + (n)*4)
#define GICD_ICACTIVER(n)(0x0380 + (n)*4)
#define GICD_IPRIORITYR(n)(0x0400 + (n)*4)
#define GICD_ITARGETSR(n)(0x0800 + (n)*4)
#define GICD_ICFGR(n)   (0x0C00 + (n)*4)
#define GICD_IGRPMODR(n)(0x0D00 + (n)*4)
#define GICD_IROUTER(n) (0x6000 + (n)*8)

/* -------------------------------------------------------
 * GICv3 Redistributor offsets
 * -------------------------------------------------------*/
#define GICR_CTLR        0x0000
#define GICR_TYPER       0x0008
#define GICR_WAKER       0x0014
#define GICR_IGROUPR0    0x0080   /* SGI/PPI frame */
#define GICR_ISENABLER0  0x0100
#define GICR_ICENABLER0  0x0180
#define GICR_ICPENDR0    0x0280
#define GICR_IPRIORITYR0 0x0400
#define GICR_ICFGR0      0x0C00
#define GICR_IGRPMODR0   0x0D00

/* A GICv3 redistributor is TWO 64 KiB frames: RD_base then SGI_base
 * => 0x20000 bytes per CPU (0x30000 on GICv4). The old default stride
 * of 0x10000 made CPU n's redistributor overlap CPU n-1's SGI frame. */
#define GICR_FRAME_SIZE  0x20000UL
#define GICR_SGI_OFFSET  0x10000UL

static __always_inline u32 gicd_read(u32 off)
{
    return READ_ONCE(*(volatile u32 *)((u8 *)g_gic.gicd_base + off));
}

static __always_inline void gicd_write(u32 off, u32 val)
{
    WRITE_ONCE(*(volatile u32 *)((u8 *)g_gic.gicd_base + off), val);
    dsb(sy);
}

/* SGI/PPI frame (IGROUPR0, ISENABLER0, IPRIORITYR...) */
static __always_inline u32 gicr_read32(u32 cpu, u32 off)
{
    u8 *base = (u8 *)g_gic.gicr_base + (u64)cpu * g_gic.gicr_stride;
    return READ_ONCE(*(volatile u32 *)(base + GICR_SGI_OFFSET + off));
}

static __always_inline void gicr_write32(u32 cpu, u32 off, u32 val)
{
    u8 *base = (u8 *)g_gic.gicr_base + (u64)cpu * g_gic.gicr_stride;
    WRITE_ONCE(*(volatile u32 *)(base + GICR_SGI_OFFSET + off), val);
    dsb(sy);
}

/* RD frame (GICR_CTLR, GICR_TYPER, GICR_WAKER). GICR_WAKER lives HERE;
 * the old code read it through the SGI frame (+0x10000) which is a
 * different register, so the wake-up handshake never happened. */
static __always_inline u32 gicr_rd_read32(u32 cpu, u32 off)
{
    u8 *base = (u8 *)g_gic.gicr_base + (u64)cpu * g_gic.gicr_stride;
    return READ_ONCE(*(volatile u32 *)(base + off));
}

static __always_inline void gicr_rd_write32(u32 cpu, u32 off, u32 val)
{
    u8 *base = (u8 *)g_gic.gicr_base + (u64)cpu * g_gic.gicr_stride;
    WRITE_ONCE(*(volatile u32 *)(base + off), val);
    dsb(sy);
}

/* wait for GICD_CTLR.RWP (register write pending) to clear */
static void gicd_wait_rwp(void)
{
    u32 timeout = 1000000;
    while ((gicd_read(GICD_CTLR) & (1U << 31)) && timeout--)
        cpu_relax();
}

/* -------------------------------------------------------
 * DAIF helpers
 * -------------------------------------------------------*/
void local_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

void local_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

u64 local_irq_save(void)
{
    u64 flags;
    __asm__ volatile(
        "mrs %0, daif    \n"
        "msr daifset, #2 \n"
        : "=r"(flags) :: "memory"
    );
    return flags;
}

void local_irq_restore(u64 flags)
{
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

bool irqs_disabled(void)
{
    u64 daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    return (daif & (1ULL << 7)) != 0;
}

/* -------------------------------------------------------
 * gic_init - initialise GICv3 from DTB-parsed addresses
 *   gicd_phys: Distributor physical base
 *   gicr_phys: Redistributor physical base
 *   gicr_stride: stride between Redistributors (DTB)
 * -------------------------------------------------------*/
int gic_init(phys_addr_t gicd_phys, phys_addr_t gicr_phys, u32 gicr_stride)
{
    extern void *phys_to_virt_early(phys_addr_t);

    if (!gicd_phys || !gicr_phys) {
        pr_err("GICv3: missing GICD/GICR address in DTB\n");
        return MX_ENODEV;
    }

    g_gic.gicd_base  = phys_to_virt_early(gicd_phys);
    g_gic.gicr_base  = phys_to_virt_early(gicr_phys);
    g_gic.gicr_stride = (gicr_stride > 0) ? gicr_stride : (u32)GICR_FRAME_SIZE;
    spin_lock_init(&g_gic.lock);

    /* Read number of IRQs from TYPER */
    u32 typer   = gicd_read(GICD_TYPER);
    u32 it_lines = typer & 0x1F;                /* ITLinesNumber */
    g_gic.nr_irqs = 32 * (it_lines + 1);
    if (g_gic.nr_irqs > 1020) g_gic.nr_irqs = 1020;

    pr_info("GICv3: GICD @ 0x%llx, GICR @ 0x%llx, %u IRQs\n",
            gicd_phys, gicr_phys, g_gic.nr_irqs);

    /* ---- Disable distributor ----------------------------*/
    gicd_write(GICD_CTLR, 0);
    gicd_wait_rwp();

    /* ---- Disable and clear all SPIs --------------------*/
    for (u32 i = 1; i < (g_gic.nr_irqs / 32); i++) {
        gicd_write(GICD_ICENABLER(i), 0xFFFFFFFF);
        gicd_write(GICD_ICPENDR(i),   0xFFFFFFFF);
        gicd_write(GICD_ICACTIVER(i), 0xFFFFFFFF);
    }

    /* ---- Set all SPIs to group 1 (NS) ------------------*/
    for (u32 i = 1; i < (g_gic.nr_irqs / 32); i++) {
        gicd_write(GICD_IGROUPR(i),   0xFFFFFFFF);
        gicd_write(GICD_IGRPMODR(i),  0x00000000);
    }

    /* ---- Set all SPI priorities to lowest priority -----*/
    for (u32 i = 8; i < (g_gic.nr_irqs / 4); i++)
        gicd_write(GICD_IPRIORITYR(i), 0xA0A0A0A0);

    /* ---- Set all SPIs level-sensitive (ICFGR=0) ---------*/
    for (u32 i = 2; i < (g_gic.nr_irqs / 16); i++)
        gicd_write(GICD_ICFGR(i), 0);
    gicd_wait_rwp();

    /* ---- Enable affinity routing + Group1 ----------------
     * GICD_CTLR: bit1 = EnableGrp1(NS), bit4 = ARE_NS.
     * (bit5 is DS, NOT the ARE bit as the old code assumed; without ARE the
     *  ICC_* system registers do not work.) */
    gicd_write(GICD_CTLR, (1U << 4) | (1U << 1));
    gicd_wait_rwp();

    return MX_OK;
}

/* -------------------------------------------------------
 * gic_cpu_init - per-CPU Redistributor setup
 * -------------------------------------------------------*/
int gic_cpu_init(void)
{
    u32 cpu = smp_processor_id();

    if (!g_gic.gicr_base) return MX_ENODEV;

    /* ---- Enable the system-register interface (ICC_SRE_EL1.SRE) ---- */
    u64 sre;
    __asm__ volatile("mrs %0, S3_0_C12_C12_5" : "=r"(sre));
    sre |= 1ULL;
    __asm__ volatile("msr S3_0_C12_C12_5, %0" :: "r"(sre) : "memory");
    isb();

    /* ---- Wake up Redistributor (GICR_WAKER is in the RD frame) ---- */
    u32 waker = gicr_rd_read32(cpu, GICR_WAKER);
    waker &= ~(1U << 1); /* clear ProcessorSleep */
    gicr_rd_write32(cpu, GICR_WAKER, waker);

    /* Poll until ChildrenAsleep == 0 */
    u32 timeout = 100000;
    bool awake = false;
    while (timeout--) {
        if (!(gicr_rd_read32(cpu, GICR_WAKER) & (1U << 2))) { awake = true; break; }
        cpu_relax();
    }

    if (!awake) {
        pr_err("GIC: CPU%u Redistributor did not wake!\n", cpu);
        return MX_ETIMEDOUT;
    }

    /* ---- SGI/PPI: group 1, lowest priority -----------*/
    gicr_write32(cpu, GICR_IGROUPR0,   0xFFFFFFFF);
    gicr_write32(cpu, GICR_IGRPMODR0,  0x00000000);
    gicr_write32(cpu, GICR_ICENABLER0, 0xFFFFFFFF);
    gicr_write32(cpu, GICR_ICPENDR0,   0xFFFFFFFF);

    for (u32 i = 0; i < 8; i++)
        gicr_write32(cpu, GICR_IPRIORITYR0 + i * 4, 0xA0A0A0A0);

    /* ---- CPU interface via system registers -----------*/
    /* Priority mask: accept all priorities */
    __asm__ volatile("msr S3_0_C4_C6_0, %0" :: "r"(0xF0ULL) : "memory");
    /* Binary point: no preemption subgrouping */
    __asm__ volatile("msr S3_0_C12_C12_3, %0" :: "r"(0ULL) : "memory");
    /* Enable Group 1 interrupts */
    __asm__ volatile("msr S3_0_C12_C12_7, %0" :: "r"(1ULL) : "memory");
    isb();

    pr_info("GIC: CPU%u online\n", cpu);
    return MX_OK;
}

/* -------------------------------------------------------
 * irq_handler_table
 * -------------------------------------------------------*/
#define MAX_IRQ_HANDLERS 1024

struct irq_handler_entry {
    irq_handler_fn  fn;
    void           *data;
    u32             irq;
    bool            valid;
};

static struct irq_handler_entry g_irq_table[MAX_IRQ_HANDLERS];
static spinlock_t               g_irq_table_lock = SPIN_LOCK_INIT;

bool gic_is_ready(void)
{
    return g_gic.gicd_base != NULL;
}

static u64 gic_this_cpu_affinity(void)
{
    u64 mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    /* Aff3[39:32] Aff2[23:16] Aff1[15:8] Aff0[7:0] -> GICD_IROUTER format */
    return (mpidr & 0x00FFFFFFULL) | (mpidr & 0xFF00000000ULL);
}

int irq_register(u32 irq, irq_handler_fn fn, void *data)
{
    if (irq >= MAX_IRQ_HANDLERS || !fn)
        return MX_EINVAL;

    u64 flags;
    spin_lock_irqsave(&g_irq_table_lock, flags);
    if (g_irq_table[irq].valid) {
        spin_unlock_irqrestore(&g_irq_table_lock, flags);
        return MX_EBUSY;
    }
    g_irq_table[irq].fn    = fn;
    g_irq_table[irq].data  = data;
    g_irq_table[irq].irq   = irq;
    g_irq_table[irq].valid = true;
    spin_unlock_irqrestore(&g_irq_table_lock, flags);

    /* A zero distributor/redistributor means gic_init() has not run (or
     * failed): the handler is recorded but nothing can be enabled. */
    if (!g_gic.gicd_base)
        return MX_OK;

    spin_lock_irqsave(&g_gic.lock, flags);
    if (irq < 32) {
        /* SGI/PPI: configured in the *redistributor* of the current CPU
         * (GICD_ISENABLER0 is RES0 when affinity routing is on). */
        u32 cpu = smp_processor_id();
        u32 pr  = gicr_read32(cpu, GICR_IPRIORITYR0 + (irq / 4) * 4);
        pr &= ~(0xFFu << ((irq & 3) * 8));
        pr |=  (0x80u << ((irq & 3) * 8));
        gicr_write32(cpu, GICR_IPRIORITYR0 + (irq / 4) * 4, pr);
        gicr_write32(cpu, GICR_ISENABLER0, 1u << irq);
    } else {
        /* SPI: priority, ROUTE to a CPU (IROUTER is undefined after reset;
         * without it the interrupt is never delivered), then enable. */
        gicd_write(GICD_IPRIORITYR(irq / 4),
                   (gicd_read(GICD_IPRIORITYR(irq / 4)) & ~(0xFFu << ((irq & 3) * 8)))
                   | (0x80u << ((irq & 3) * 8)));
        u64 aff = gic_this_cpu_affinity();
        WRITE_ONCE(*(volatile u64 *)((u8 *)g_gic.gicd_base + GICD_IROUTER(irq)), aff);
        dsb(sy);
        gicd_write(GICD_ISENABLER(irq / 32), 1u << (irq & 31));
    }
    spin_unlock_irqrestore(&g_gic.lock, flags);

    return MX_OK;
}

int irq_unregister(u32 irq)
{
    if (irq >= MAX_IRQ_HANDLERS)
        return MX_EINVAL;

    u64 flags;
    if (g_gic.gicd_base) {
        spin_lock_irqsave(&g_gic.lock, flags);
        if (irq < 32)
            gicr_write32(smp_processor_id(), GICR_ICENABLER0, 1u << irq);
        else
            gicd_write(GICD_ICENABLER(irq / 32), 1u << (irq & 31));
        spin_unlock_irqrestore(&g_gic.lock, flags);
    }

    spin_lock_irqsave(&g_irq_table_lock, flags);
    g_irq_table[irq].valid = false;
    g_irq_table[irq].fn    = NULL;
    g_irq_table[irq].data  = NULL;
    spin_unlock_irqrestore(&g_irq_table_lock, flags);

    return MX_OK;
}

/* -------------------------------------------------------
 * handle_irq - called from exception vector
 * -------------------------------------------------------*/
void handle_irq(struct pt_regs *regs)
{
    (void)regs;
    u64 irqstat;

    /* Acknowledge the interrupt */
    __asm__ volatile("mrs %0, S3_0_C12_C12_0" : "=r"(irqstat));
    u32 irq = (u32)(irqstat & 0xFFFFFF);

    /* 1020..1023 are special INTIDs (spurious etc.): nothing to EOI */
    if (irq >= 1020)
        return;

    if (irq < MAX_IRQ_HANDLERS && g_irq_table[irq].valid && g_irq_table[irq].fn)
        g_irq_table[irq].fn(irq, g_irq_table[irq].data);
    else
        pr_warn("IRQ: unhandled IRQ %u\n", irq);

    /* End of interrupt. ICC_CTLR_EL1.EOImode is 0 (priority drop +
     * deactivate in one write), so writing ICC_DIR_EL1 as well - as the old
     * code did - is wrong. */
    __asm__ volatile("msr S3_0_C12_C12_1, %0" :: "r"(irqstat) : "memory");
    isb();
}

void handle_fiq(struct pt_regs *regs)
{
    (void)regs;
    pr_warn("IRQ: unexpected FIQ\n");
}

void handle_serror(struct pt_regs *regs)
{
    u64 esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    monox_panic("SError (System Error) exception! ESR=0x%llx ELR=0x%llx\n",
                esr, regs ? regs->elr : 0);
}

void handle_bad_exception(void)
{
    u64 esr, elr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    monox_panic("Unhandled exception (SP0 or AArch32) ESR=0x%llx ELR=0x%llx\n",
                esr, elr);
}

/* -------------------------------------------------------
 * Synchronous exceptions
 *
 * handle_sync_exception / handle_el0_sync were referenced by vectors.S but
 * defined nowhere (link error). Both now decode ESR_EL1 and report the
 * fault precisely instead of silently looping.
 * -------------------------------------------------------*/
static const char *esr_class_name(u32 ec)
{
    switch (ec) {
    case 0x00: return "Unknown reason";
    case 0x01: return "Trapped WFI/WFE";
    case 0x07: return "FP/SIMD access trapped";
    case 0x0E: return "Illegal execution state";
    case 0x15: return "SVC (AArch64)";
    case 0x18: return "Trapped MSR/MRS/system instruction";
    case 0x20: return "Instruction abort (lower EL)";
    case 0x21: return "Instruction abort (same EL)";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data abort (lower EL)";
    case 0x25: return "Data abort (same EL)";
    case 0x26: return "SP alignment fault";
    case 0x2F: return "SError";
    case 0x30: return "Breakpoint (lower EL)";
    case 0x31: return "Breakpoint (same EL)";
    case 0x3C: return "BRK instruction";
    default:   return "Other";
    }
}

static void __noreturn sync_fault(const char *where, struct pt_regs *regs)
{
    u64 esr, far;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    u32 ec = (u32)((esr >> 26) & 0x3F);

    pr_crit("%s: %s (EC=0x%02x ISS=0x%07llx)\n", where,
            esr_class_name(ec), ec, esr & 0x1FFFFFF);
    pr_crit("  ESR=0x%016llx FAR=0x%016llx\n", esr, far);
    if (regs) {
        pr_crit("  ELR=0x%016llx SPSR=0x%016llx\n", regs->elr, regs->spsr);
        pr_crit("  x0=0x%016llx x1=0x%016llx x29=0x%016llx x30=0x%016llx\n",
                regs->regs[0], regs->regs[1], regs->regs[29], regs->regs[30]);
    }
    monox_panic("Unrecoverable synchronous exception at %s\n", where);
}

void handle_sync_exception(struct pt_regs *regs)
{
    sync_fault("kernel", regs);
}

void handle_el0_sync(struct pt_regs *regs)
{
    /* No user-space exists in this kernel build yet; any EL0 trap is fatal. */
    sync_fault("el0", regs);
}
