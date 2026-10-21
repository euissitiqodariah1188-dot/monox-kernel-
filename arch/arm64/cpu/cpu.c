/* MONOX Kernel - ARM64 CPU Feature Detection & Management
 * Supports ARMv8.0 through ARMv9.2
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "cpu.h"

/* -------------------------------------------------------
 * Per-CPU info table (populated at boot)
 * -------------------------------------------------------*/
static struct cpu_info g_cpu[MONOX_MAX_CPUS];
static u32             g_nr_cpus = 0;
static spinlock_t      g_cpu_lock = SPIN_LOCK_INIT;

/* -------------------------------------------------------
 * Read system registers
 * -------------------------------------------------------*/
#define READ_SYSREG(reg) \
    ({ u64 _v; __asm__ volatile("mrs %0, " #reg : "=r"(_v)); _v; })

#define WRITE_SYSREG(val, reg) \
    __asm__ volatile("msr " #reg ", %0" :: "r"((u64)(val)) : "memory")

/* -------------------------------------------------------
 * cpu_detect_features
 *   Called very early - no printk yet, just populate struct
 * -------------------------------------------------------*/
void cpu_detect_features(void)
{
    u32 cpu = smp_processor_id();          /* already clamped < MONOX_MAX_CPUS */
    struct cpu_info *info = &g_cpu[cpu];

    memset(info, 0, sizeof(*info));
    info->cpu_id = cpu;

    /* ---- MIDR_EL1: Implementer, Variant, Architecture, PartNum, Rev ----*/
    u64 midr = READ_SYSREG(midr_el1);
    info->implementer    = (midr >> 24) & 0xFF;
    info->variant        = (midr >> 20) & 0xF;
    info->arch_revision  = (midr >> 16) & 0xF;
    info->part_number    = (midr >>  4) & 0xFFF;
    info->revision       = (midr      ) & 0xF;

    /* ---- MPIDR_EL1: affinity / cluster topology -----*/
    u64 mpidr = READ_SYSREG(mpidr_el1);
    info->aff0 = (mpidr >>  0) & 0xFF;
    info->aff1 = (mpidr >>  8) & 0xFF;
    info->aff2 = (mpidr >> 16) & 0xFF;
    info->aff3 = (mpidr >> 32) & 0xFF;
    info->is_mt = (mpidr >> 24) & 1;

    /* ---- ID_AA64PFR0_EL1: SVE, FP, AdvSIMD, GIC ----*/
    u64 pfr0 = READ_SYSREG(id_aa64pfr0_el1);
    info->has_fp      = ((pfr0 >>  16) & 0xF) != 0xF;
    info->has_simd    = ((pfr0 >>  20) & 0xF) != 0xF;
    info->has_sve     = ((pfr0 >>  32) & 0xF) != 0;
    info->has_gic     = ((pfr0 >>  24) & 0xF) != 0;

    /* ---- ID_AA64PFR1_EL1: MTE, BT, SSBS --------------*/
    u64 pfr1 = READ_SYSREG(id_aa64pfr1_el1);
    info->has_mte     = ((pfr1 >>  8) & 0xF) != 0;
    info->has_bti     = ((pfr1 >>  0) & 0xF) != 0;
    info->has_ssbs    = ((pfr1 >>  4) & 0xF) != 0;

    /* ---- ID_AA64ISAR0_EL1: Crypto, CRC32, Atomic -----*/
    u64 isar0 = READ_SYSREG(id_aa64isar0_el1);
    info->has_aes     = ((isar0 >>  4) & 0xF) != 0;
    info->has_sha1    = ((isar0 >>  8) & 0xF) != 0;
    info->has_sha256  = ((isar0 >> 12) & 0xF) != 0;
    info->has_sha512  = ((isar0 >> 12) & 0xF) >= 2;
    info->has_crc32   = ((isar0 >> 16) & 0xF) != 0;
    info->has_atomics = ((isar0 >> 20) & 0xF) != 0;
    info->has_rdm     = ((isar0 >> 28) & 0xF) != 0;

    /* ---- ID_AA64ISAR1_EL1: DPB, JSCVT, FRINTTS, LRCPC */
    u64 isar1 = READ_SYSREG(id_aa64isar1_el1);
    info->has_dpb     = ((isar1 >>  0) & 0xF) != 0;
    info->has_jscvt   = ((isar1 >> 12) & 0xF) != 0;   /* JSCVT is [15:12]; [11:8] is FCMA */
    info->has_lrcpc   = ((isar1 >> 20) & 0xF) != 0;
    info->has_frintts = ((isar1 >> 32) & 0xF) != 0;

    /* ---- ID_AA64MMFR0_EL1: Physical address range ----*/
    u64 mmfr0 = READ_SYSREG(id_aa64mmfr0_el1);
    info->pa_range    = (u8)(mmfr0 & 0xF);
    /* PA range: 0=32b 1=36b 2=40b 3=42b 4=44b 5=48b 6=52b */

    /* ---- ID_AA64DFR0_EL1: Debug, PMU ------------------*/
    u64 dfr0 = READ_SYSREG(id_aa64dfr0_el1);
    info->pmu_version = (u8)((dfr0 >> 8) & 0xF);
    info->dbg_version = (u8)((dfr0 >> 0) & 0xF);

    /* ---- Determine ARM architecture version -----------
     * ID_AA64PFR0_EL1[7:4] is the EL1 *handling* field, NOT the architecture
     * version, so the old test never reported ARMv9. ARMv9.0 mandates SVE2:
     * ID_AA64ZFR0_EL1.SVEver (bits [3:0]) >= 1. The register is only
     * defined when SVE is implemented (read via its raw encoding so older
     * assemblers accept it). */
    info->arch = CPU_ARCH_UNKNOWN;
    if (((pfr0 >> 0) & 0xF) != 0)           /* EL0 implemented => at least ARMv8 */
        info->arch = CPU_ARCH_V8;
    if (info->has_sve) {
        u64 zfr0;
        __asm__ volatile("mrs %0, S3_0_C0_C4_4" : "=r"(zfr0)); /* ID_AA64ZFR0_EL1 */
        if ((zfr0 & 0xF) >= 1)
            info->arch = CPU_ARCH_V9;
    }

    info->online = true;

    spin_lock(&g_cpu_lock);
    if (cpu >= g_nr_cpus)
        g_nr_cpus = cpu + 1;
    spin_unlock(&g_cpu_lock);
}

/* -------------------------------------------------------
 * cpu_print_info  - called after printk is up
 * -------------------------------------------------------*/
void cpu_print_info(u32 cpu)
{
    const struct cpu_info *info = cpu_get_info(cpu);
    if (!info) return;

    pr_info("CPU%u: Implementer=0x%02x Part=0x%03x Arch=%s\n",
            cpu, info->implementer, info->part_number,
            info->arch == CPU_ARCH_V9 ? "ARMv9" :
            info->arch == CPU_ARCH_V8 ? "ARMv8" : "?");

    pr_info("CPU%u: FP=%d SIMD=%d SVE=%d AES=%d SHA256=%d MTE=%d BTI=%d\n",
            cpu,
            info->has_fp, info->has_simd, info->has_sve,
            info->has_aes, info->has_sha256,
            info->has_mte, info->has_bti);

    static const u8 pa_range_bits[] = { 32,36,40,42,44,48,52 };
    u8 pa = (info->pa_range < 7) ? pa_range_bits[info->pa_range] : 0;
    pr_info("CPU%u: PA=%u-bit Atomics=%d LRCPC=%d PMUv%d\n",
            cpu, pa, info->has_atomics, info->has_lrcpc,
            info->pmu_version);
}

/* -------------------------------------------------------
 * cpu_get_info / cpu_count
 * -------------------------------------------------------*/
struct cpu_info *cpu_get_info(u32 cpu)
{
    if (cpu >= MONOX_MAX_CPUS)
        return NULL;
    return &g_cpu[cpu];
}

u32 cpu_count(void)
{
    return READ_ONCE(g_nr_cpus);
}

/* -------------------------------------------------------
 * cpu_configure_el1 - set up EL1 system registers
 *   Must be called on every CPU after feature detection
 * -------------------------------------------------------*/
void cpu_configure_el1(void)
{
    u32 cpu = smp_processor_id();
    const struct cpu_info *info = &g_cpu[cpu];

    /* ---- SCTLR_EL1 ------------------------------------------------
     * The MMU itself is switched on by mmu_enable() on the boot CPU and by
     * secondary_entry (head.S) on the others, once TTBR/TCR/MAIR are
     * programmed. The old code set M here unconditionally, which on a
     * secondary CPU (no translation regs programmed yet) is fatal.
     * RES1 bits (28 nTLSMD, 29 LSMAOE) must NOT be cleared.            */
    u64 sctlr = READ_SYSREG(sctlr_el1);
    sctlr |=  (1ULL <<  2);  /* C  - D-cache enable                        */
    sctlr |=  (1ULL << 12);  /* I  - I-cache enable                        */
    sctlr |=  (1ULL << 23);  /* SPAN=1 - do NOT force PAN on exceptions    */
    sctlr &= ~(1ULL << 19);  /* WXN=0                                      */
    if (info->has_bti)
        sctlr |= (1ULL << 35); /* BT0 - BTI enforcement for EL0 (bit 35)   */
    WRITE_SYSREG(sctlr, sctlr_el1);
    isb();

    /* ---- CPACR_EL1: allow FP/SIMD in EL1 & EL0 -----------------*/
    u64 cpacr = READ_SYSREG(cpacr_el1);
    cpacr |= (3ULL << 20); /* FPEN = 11 (allow both ELs) */
    if (info->has_sve)
        cpacr |= (3ULL << 16); /* ZEN  = 11 (SVE enabled) */
    WRITE_SYSREG(cpacr, cpacr_el1);
    isb();

    /* ---- MDSCR_EL1: debug / PMU --------------------------------*/
    u64 mdscr = READ_SYSREG(mdscr_el1);
    mdscr &= ~(1ULL << 0); /* MDE=0 (debug events disabled by default) */
    WRITE_SYSREG(mdscr, mdscr_el1);

    /* ---- CTR_EL0 (read-only): cache line size ------------------*/
    /* Stored in cpu_info by caller after this function */
}

/* -------------------------------------------------------
 * cpu_cache_line_size
 * -------------------------------------------------------*/
u32 cpu_cache_line_size(void)
{
    u64 ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    /* DminLine field [19:16] = log2(# words); 1 word = 4 bytes */
    return 4 << ((ctr >> 16) & 0xF);
}

/* -------------------------------------------------------
 * cpu_wfi / cpu_wfe - low-power wait
 * -------------------------------------------------------*/
void cpu_wfi(void)
{
    __asm__ volatile("wfi" ::: "memory");
}

void cpu_wfe(void)
{
    __asm__ volatile("wfe" ::: "memory");
}

/* -------------------------------------------------------
 * cpu_sev - signal event to all CPUs (wake WFE)
 * -------------------------------------------------------*/
void cpu_sev(void)
{
    dsb(ish);
    __asm__ volatile("sev" ::: "memory");
}

/* -------------------------------------------------------
 * PSCI conduit. The DTB's psci/method says whether firmware is reached with
 * SMC (real hardware, QEMU with virtualization=on) or HVC (QEMU -M virt
 * booted at EL1: an SMC there is an UNDEFINED instruction).
 * -------------------------------------------------------*/
static bool g_psci_hvc = false;

void cpu_set_psci_conduit(bool hvc)
{
    g_psci_hvc = hvc;
}

static void psci_call_noreturn(u64 fn)
{
    register u64 x0 __asm__("x0") = fn;
    if (g_psci_hvc)
        __asm__ volatile("hvc #0" : "+r"(x0) :: "memory", "x1", "x2", "x3");
    else
        __asm__ volatile("smc #0" : "+r"(x0) :: "memory", "x1", "x2", "x3");
}

/* -------------------------------------------------------
 * cpu_reset - PSCI SYSTEM_RESET
 * -------------------------------------------------------*/
void __noreturn cpu_reset(void)
{
    psci_call_noreturn(0x84000009ULL);      /* PSCI_SYSTEM_RESET */
    /* If PSCI not available, spin */
    hang();
    __builtin_unreachable();
}

/* -------------------------------------------------------
 * cpu_poweroff - PSCI SYSTEM_OFF
 * -------------------------------------------------------*/
void __noreturn cpu_poweroff(void)
{
    psci_call_noreturn(0x84000008ULL);      /* PSCI_SYSTEM_OFF */
    hang();
    __builtin_unreachable();
}
