/* MONOX Kernel - ARM64 CPU Info Header */

#ifndef _MONOX_ARM64_CPU_H
#define _MONOX_ARM64_CPU_H

#include <kernel/types.h>
#include <kernel/kernel.h>

typedef enum {
    CPU_ARCH_UNKNOWN = 0,
    CPU_ARCH_V8      = 8,
    CPU_ARCH_V9      = 9,
} cpu_arch_t;

struct cpu_info {
    u32  cpu_id;

    /* MIDR */
    u8   implementer;
    u8   variant;
    u8   arch_revision;
    u16  part_number;
    u8   revision;

    /* MPIDR */
    u8   aff0, aff1, aff2, aff3;
    bool is_mt;

    /* Architecture version */
    cpu_arch_t arch;

    /* Feature flags */
    bool has_fp;
    bool has_simd;
    bool has_sve;
    bool has_gic;
    bool has_mte;
    bool has_bti;
    bool has_ssbs;

    /* Crypto */
    bool has_aes;
    bool has_sha1;
    bool has_sha256;
    bool has_sha512;
    bool has_crc32;
    bool has_atomics;
    bool has_rdm;

    /* Misc ISA */
    bool has_dpb;
    bool has_jscvt;
    bool has_lrcpc;
    bool has_frintts;

    /* Memory */
    u8   pa_range;      /* 0-6, see ID_AA64MMFR0_EL1 */

    /* Debug / PMU */
    u8   pmu_version;
    u8   dbg_version;

    /* State */
    bool online;
};

/* Public API */
void            cpu_detect_features(void);
void            cpu_configure_el1(void);
void            cpu_print_info(u32 cpu);
struct cpu_info *cpu_get_info(u32 cpu);
u32             cpu_count(void);
u32             cpu_cache_line_size(void);
void            cpu_wfi(void);
void            cpu_wfe(void);
void            cpu_sev(void);
void            cpu_set_psci_conduit(bool hvc);   /* from the DTB psci node */
void __noreturn cpu_reset(void);
void __noreturn cpu_poweroff(void);

/* Defined in head.S */
extern void hang(void) __noreturn;
extern volatile u64 secondary_spin_table[];

#endif /* _MONOX_ARM64_CPU_H */
