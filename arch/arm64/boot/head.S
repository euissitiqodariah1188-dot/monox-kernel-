/* MONOX Kernel - ARM64 Boot Entry
 * Supports ARMv8-A (AArch64) through ARMv9-A
 *
 * Entry: CPU is in EL2 or EL1, MMU off, caches off.
 *        x0 = physical address of DTB (device tree blob)
 *        x1-x3 = 0 (reserved by boot protocol)
 */

#include "asm_macro.h"

    .section ".head.text", "ax"
    .global _start
    .balign 0x1000      /* 4 KiB aligned (required by UEFI loader) */

/* -------------------------------------------------------
 * Kernel image header (compatible with Linux ARM64 Image)
 * -------------------------------------------------------*/
_start:
    b   primary_entry           /* branch to entry        */
    .long 0                     /* reserved               */
    .quad 0                     /* image load offset      */
    .quad __kernel_size         /* image size             */
    .quad 0x000A               /* flags: LE, 4K pages    */
    .quad 0                     /* reserved               */
    .quad 0                     /* reserved               */
    .quad 0                     /* reserved               */
    .ascii "ARM\x64"            /* magic                  */
    .long 0                     /* reserved               */

/* -------------------------------------------------------
 * primary_entry - executed on the primary (boot) CPU
 * -------------------------------------------------------*/
    .section ".init.text", "ax"
primary_entry:
    dbg_putc 0x31                /* alive: reached the entry point */
    /* ---- Save DTB pointer --------------------------------*/
    adr_l   x28, __dtb_phys    /* x28 = &__dtb_phys */
    str     x0, [x28]          /* save DTB phys addr */

    /* ---- Save the physical address the image was loaded at ----
     * (PC-relative => independent of where the bootloader put us) */
    adr_l   x1, _start
    adr_l   x2, __kernel_phys_base
    str     x1, [x2]

    /* ---- Disable all interrupts -------------------------*/
    msr     daifset, #0xf

    /* ---- Detect current EL ------------------------------*/
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    beq     el2_entry
    cmp     x0, #1
    beq     el1_entry
    /* EL0 is invalid for a kernel, halt */
    b       hang

/* -------------------------------------------------------
 * el2_entry - drop to EL1 with HVC
 * -------------------------------------------------------*/
el2_entry:
    /* Set HCR_EL2: RW=1 (EL1 is AArch64), TGE=0 */
    mov     x0, #(1 << 31)     /* RW bit */
    msr     hcr_el2, x0

    /* EL1 must see the real MIDR/MPIDR (VPIDR/VMPIDR reset to UNKNOWN) */
    mrs     x0, midr_el1
    msr     vpidr_el2, x0
    mrs     x0, mpidr_el1
    msr     vmpidr_el2, x0

    /* Do not trap EL1 physical counter/timer accesses; zero virtual offset */
    mov     x0, #3
    msr     cnthctl_el2, x0
    msr     cntvoff_el2, xzr

    /* Do not trap FP/SIMD/SVE or trace accesses from EL1 */
    mov     x0, #0x33ff
    msr     cptr_el2, x0
    msr     hstr_el2, xzr
    msr     mdcr_el2, xzr

    /* GICv3 system-register interface: let EL1 use ICC_*_EL1 (otherwise
     * ICC_SRE_EL1 accesses trap to EL2 and the kernel hangs in gic_cpu_init) */
    mrs     x0, id_aa64pfr0_el1
    ubfx    x0, x0, #24, #4
    cbz     x0, 1f
    mov     x0, #((1 << 0) | (1 << 3))      /* SRE | Enable */
    msr     S3_4_C12_C9_5, x0               /* ICC_SRE_EL2 */
    isb
    mrs     x0, S3_4_C12_C9_5
    tbz     x0, #0, 1f
    msr     S3_4_C12_C11_0, xzr             /* ICH_HCR_EL2 = 0 */
1:

    /* Fake an ERET to EL1 */
    adr     x0, el1_entry
    msr     elr_el2, x0
    mov     x0, #0x3c5         /* M[4:0]=EL1h, DAIF masked */
    msr     spsr_el2, x0
    eret

/* -------------------------------------------------------
 * el1_entry - kernel runs in EL1
 * -------------------------------------------------------*/
el1_entry:
    /* ---- Set up EL1 stack pointer ----------------------*/
    msr     SPSel, #1              /* use SP_EL1              */
    adr_l   x0, __boot_stack_top  /* top of init stack       */
    mov     sp, x0

    /* ---- SCTLR_EL1 := architecturally clean "MMU off" value --
     * RES1 bits set; M, C, I, WXN, EE, E0E, A, SA... all clear.
     * (the old code only cleared M/C/I and left every other bit UNKNOWN) */
    movz    x0, #0x0980
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0
    isb

    /* ---- Zero BSS ---------------------------------------*/
    adr_l   x0, __bss_start
    adr_l   x1, __bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:

    /* ---- Copy data section from ROM to RAM (if needed) -*/
    adr_l   x0, __data_start
    adr_l   x1, __data_load     /* LMA (load address)       */
    adr_l   x2, __data_end
    cmp     x0, x1              /* already in place?         */
    beq     3f
1:  cmp     x0, x2
    b.hs    3f
    ldr     x3, [x1], #8
    str     x3, [x0], #8
    b       1b
3:

    dbg_putc 0x32                /* BSS cleared */

    /* ---- Set VBAR_EL1 (vector table base) --------------
     * Physical (low) alias for now: the MMU is still off. monox_start_virt()
     * switches VBAR to the high alias once the kernel runs there. */
    adr_l   x0, monox_vectors
    msr     vbar_el1, x0
    isb

    /* ---- Detect CPU features (ARMv8 vs ARMv9) ----------*/
    bl      cpu_detect_features

    /* ---- Set up early UART for debug output ------------*/
    bl      early_uart_init

    dbg_putc 0x33                /* about to enter C */

    /* ---- Jump to C kernel main -------------------------*/
    adr_l   x0, __dtb_phys
    ldr     x0, [x0]
    bl      monox_start         /* noreturn */

/* -------------------------------------------------------
 * Secondary CPU entry (SMP)
 * -------------------------------------------------------*/
    .global secondary_entry
secondary_entry:
    msr     daifset, #0xf

    /* Wait for primary to publish this CPU's stack in its own slot
     * (the old code always polled slot 0, so no secondary ever started) */
    mrs     x3, mpidr_el1
    and     x3, x3, #0xF
    adr_l   x0, secondary_spin_table
    add     x0, x0, x3, lsl #3
1:  ldar    x2, [x0]
    cbnz    x2, 2f
    wfe
    b       1b
2:
    msr     SPSel, #1
    mov     sp, x2              /* stack set by primary      */

    /* Clean MMU-off SCTLR, then program the same translation setup the
     * primary CPU uses (values were saved by mmu_enable()) and turn the
     * MMU + caches on. All of this is position independent (adr_l). */
    movz    x0, #0x0980
    movk    x0, #0x30d0, lsl #16
    msr     sctlr_el1, x0
    isb
    adr_l   x1, secondary_mmu_regs
    ldp     x2, x3, [x1]        /* mair, tcr   */
    ldp     x4, x5, [x1, #16]   /* ttbr, sctlr */
    cbz     x4, 3f              /* primary never enabled the MMU -> skip */
    msr     mair_el1, x2
    msr     tcr_el1, x3
    msr     ttbr0_el1, x4
    msr     ttbr1_el1, x4
    isb
    tlbi    vmalle1
    dsb     nsh
    isb
    msr     sctlr_el1, x5
    isb
3:
    /* The kernel runs at its HIGH link address (see __enter_virt): use the
     * absolute address of the vectors and of the C entry point, not the
     * PC-relative (low) ones. */
    mov_q   x0, monox_vectors
    msr     vbar_el1, x0
    isb

    mov_q   x9, secondary_start
    br      x9                  /* per-CPU init in C (never returns) */

/* -------------------------------------------------------
 * __enter_virt - leave the physical (low) alias and continue in
 *                monox_start_virt() at its HIGH link-time address.
 *   x0 = dtb physical address (passed through unchanged)
 *   x1 = KERNEL_IMAGE_VMA - kernel physical base
 *
 * Called with the MMU already ON and the image mapped at both aliases.
 * Why this exists: the C code is linked at 0xFFFFFFC0_xxxxxxxx, so every
 * statically initialised pointer (LIST_HEAD_INIT, string tables, ops
 * tables...) holds a HIGH address, while adrp/PC-relative code that is
 * still running at the low alias computes LOW addresses for the same
 * objects. Comparing the two (list_empty(), pos != head) silently fails.
 * Running at the link address everywhere removes the mix entirely (this is
 * what Linux does in __primary_switch).
 * -------------------------------------------------------*/
    .section ".init.text", "ax"
    .global __enter_virt
__enter_virt:
    mov_q   x9, monox_start_virt    /* absolute high address     */
    mov     x3, sp
    add     sp, x3, x1              /* boot stack -> high alias  */
    br      x9

/* -------------------------------------------------------
 * Hang (unrecoverable)
 * -------------------------------------------------------*/
    .global hang
hang:
    msr     daifset, #0xf
    wfi
    b       hang

/* -------------------------------------------------------
 * Data
 * -------------------------------------------------------*/
    .section ".init.data", "aw"
    .global __dtb_phys
__dtb_phys:
    .quad   0

    .global secondary_spin_table
    .balign 8
secondary_spin_table:
    .space  (16 * 8)  /* 16 CPUs * 8 bytes each */

    /* {mair, tcr, ttbr, sctlr} filled in by mmu_enable() for secondaries */
    .global secondary_mmu_regs
    .balign 16
secondary_mmu_regs:
    .space  (4 * 8)

    /* physical address the image was loaded at (set by primary_entry) */
    .global __kernel_phys_base
    .balign 8
__kernel_phys_base:
    .quad   0

    /* 16 KiB boot stack for primary CPU */
    .section ".init.stack", "aw"
    .balign 16
    .space  0x4000
__boot_stack_top:
