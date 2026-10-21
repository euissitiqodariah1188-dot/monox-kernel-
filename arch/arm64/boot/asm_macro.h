/* MONOX Kernel - ARM64 Assembly Macro Helpers */

#ifndef _MONOX_ASM_MACRO_H
#define _MONOX_ASM_MACRO_H

/*
 * adr_l  reg, sym
 *   Load the absolute address of a symbol into a register,
 *   works regardless of the image offset.
 */
.macro  adr_l, reg, sym
    adrp    \reg, \sym
    add     \reg, \reg, :lo12:\sym
.endm

/*
 * mov_q  reg, sym
 *   Load a 64-bit ABSOLUTE (link-time) address, e.g. the high virtual
 *   address of a symbol. Unlike adr_l this does NOT depend on the PC, so it
 *   yields 0xFFFFFFC0_xxxxxxxx even while the CPU still runs at the physical
 *   (low) alias. Four fixed instructions (the old version tried to skip
 *   zero chunks with .ifnc on macro text, which never worked).
 */
.macro  mov_q, reg, val
    movz    \reg, #:abs_g3:\val
    movk    \reg, #:abs_g2_nc:\val
    movk    \reg, #:abs_g1_nc:\val
    movk    \reg, #:abs_g0_nc:\val
.endm

/*
 * dbg_putc  ch
 *   Emit one character on the PL011 at EARLY_CON_BASE with the MMU OFF
 *   (physical address). Compiles to nothing unless the kernel is built with
 *   `make EARLY_CON=0x09000000` (QEMU virt). Lets you see how far the boot
 *   got even if the C console never comes up.
 */
.macro  dbg_putc, ch
#ifdef EARLY_CON_BASE
    movz    x16, #(EARLY_CON_BASE & 0xffff)
    movk    x16, #((EARLY_CON_BASE >> 16) & 0xffff), lsl #16
    movz    w17, #\ch
    strb    w17, [x16]
#endif
.endm

/*
 * kernel_entry / kernel_exit
 *   Save / restore all GPRs on entry to / exit from exception handler.
 *   Frame size = 34 * 8 = 272 bytes (x0-x30 + elr + spsr + pad).
 */
.macro  kernel_entry, el
    sub     sp,  sp,  #272
    stp     x0,  x1,  [sp, #(0  * 8)]
    stp     x2,  x3,  [sp, #(2  * 8)]
    stp     x4,  x5,  [sp, #(4  * 8)]
    stp     x6,  x7,  [sp, #(6  * 8)]
    stp     x8,  x9,  [sp, #(8  * 8)]
    stp     x10, x11, [sp, #(10 * 8)]
    stp     x12, x13, [sp, #(12 * 8)]
    stp     x14, x15, [sp, #(14 * 8)]
    stp     x16, x17, [sp, #(16 * 8)]
    stp     x18, x19, [sp, #(18 * 8)]
    stp     x20, x21, [sp, #(20 * 8)]
    stp     x22, x23, [sp, #(22 * 8)]
    stp     x24, x25, [sp, #(24 * 8)]
    stp     x26, x27, [sp, #(26 * 8)]
    stp     x28, x29, [sp, #(28 * 8)]
    str     x30,      [sp, #(30 * 8)]
    .if     \el == 1
    mrs     x20, elr_el1
    mrs     x21, spsr_el1
    .else
    mrs     x20, elr_el2
    mrs     x21, spsr_el2
    .endif
    stp     x20, x21, [sp, #(31 * 8)]
.endm

.macro  kernel_exit, el
    ldp     x20, x21, [sp, #(31 * 8)]
    .if     \el == 1
    msr     elr_el1,  x20
    msr     spsr_el1, x21
    .else
    msr     elr_el2,  x20
    msr     spsr_el2, x21
    .endif
    ldp     x0,  x1,  [sp, #(0  * 8)]
    ldp     x2,  x3,  [sp, #(2  * 8)]
    ldp     x4,  x5,  [sp, #(4  * 8)]
    ldp     x6,  x7,  [sp, #(6  * 8)]
    ldp     x8,  x9,  [sp, #(8  * 8)]
    ldp     x10, x11, [sp, #(10 * 8)]
    ldp     x12, x13, [sp, #(12 * 8)]
    ldp     x14, x15, [sp, #(14 * 8)]
    ldp     x16, x17, [sp, #(16 * 8)]
    ldp     x18, x19, [sp, #(18 * 8)]
    ldp     x20, x21, [sp, #(20 * 8)]
    ldp     x22, x23, [sp, #(22 * 8)]
    ldp     x24, x25, [sp, #(24 * 8)]
    ldp     x26, x27, [sp, #(26 * 8)]
    ldp     x28, x29, [sp, #(28 * 8)]
    ldr     x30,      [sp, #(30 * 8)]
    add     sp,  sp,  #272
    eret
    dsb     nsh                 /* speculation barrier after ERET */
    isb
.endm

#endif /* _MONOX_ASM_MACRO_H */
