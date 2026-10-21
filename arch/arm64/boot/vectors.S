/* MONOX Kernel - ARM64 Exception Vector Table
 *
 * Structure per ARM ARM D1.10:
 *   Offset 0x000 : Current EL, SP0  - Sync
 *   Offset 0x080 : Current EL, SP0  - IRQ
 *   Offset 0x100 : Current EL, SP0  - FIQ
 *   Offset 0x180 : Current EL, SP0  - SError
 *   Offset 0x200 : Current EL, SPx  - Sync
 *   Offset 0x280 : Current EL, SPx  - IRQ
 *   Offset 0x300 : Current EL, SPx  - FIQ
 *   Offset 0x380 : Current EL, SPx  - SError
 *   Offset 0x400 : Lower  EL, AArch64 - Sync
 *   Offset 0x480 : Lower  EL, AArch64 - IRQ
 *   Offset 0x500 : Lower  EL, AArch64 - FIQ
 *   Offset 0x580 : Lower  EL, AArch64 - SError
 *   Offset 0x600 : Lower  EL, AArch32 - Sync
 *   Offset 0x680 : Lower  EL, AArch32 - IRQ
 *   Offset 0x700 : Lower  EL, AArch32 - FIQ
 *   Offset 0x780 : Lower  EL, AArch32 - SError
 *
 * BUG FIX: every slot is only 0x80 bytes (32 instructions). The previous
 * version put the whole register save / C call / restore sequence (~42
 * instructions) inside each slot, so each slot overflowed into the next
 * one and, because of ".balign 0x80", every entry after the first landed
 * at the wrong architectural offset - the CPU then executed the middle of
 * another handler on the first interrupt or fault.
 * Each slot now contains a single branch; the handlers live below the table.
 */

#include "asm_macro.h"

    .section ".vectors", "ax"
    .balign 0x800       /* VBAR must be 2 KiB aligned */

    .macro  ventry, label
    .balign 0x80
    b       \label
    .endm

    .global monox_vectors
monox_vectors:
    /* Current EL with SP0 */
    ventry  el1_sp0_sync
    ventry  el1_sp0_irq
    ventry  el1_sp0_fiq
    ventry  el1_sp0_serror
    /* Current EL with SPx (kernel mode) */
    ventry  el1_spx_sync
    ventry  el1_spx_irq
    ventry  el1_spx_fiq
    ventry  el1_spx_serror
    /* Lower EL AArch64 (user mode) */
    ventry  el0_sync
    ventry  el0_irq
    ventry  el0_fiq
    ventry  el0_serror
    /* Lower EL AArch32 (legacy, treated as bad) */
    ventry  el0_32_sync
    ventry  el0_32_irq
    ventry  el0_32_fiq
    ventry  el0_32_serror

    /* The table must be exactly 16 * 0x80 = 0x800 bytes long */
    .balign 0x80

/* -------------------------------------------------------
 * Handlers (outside the 128-byte slots)
 *   handler <label>, <c function>, <pass pt_regs in x0?>
 * -------------------------------------------------------*/
    .macro  vhandler, label, fn, with_regs
    .balign 0x40
\label:
    kernel_entry 1
    .if     \with_regs
    mov     x0, sp
    .endif
    bl      \fn
    kernel_exit 1
    .endm

    /* SP0 and AArch32 are never valid for this kernel */
    vhandler el1_sp0_sync,    handle_bad_exception,  0
    vhandler el1_sp0_irq,     handle_bad_exception,  0
    vhandler el1_sp0_fiq,     handle_bad_exception,  0
    vhandler el1_sp0_serror,  handle_bad_exception,  0

    vhandler el1_spx_sync,    handle_sync_exception, 1
    vhandler el1_spx_irq,     handle_irq,            1
    vhandler el1_spx_fiq,     handle_fiq,            1
    vhandler el1_spx_serror,  handle_serror,         1

    vhandler el0_sync,        handle_el0_sync,       1
    vhandler el0_irq,         handle_irq,            1
    vhandler el0_fiq,         handle_fiq,            1
    vhandler el0_serror,      handle_serror,         1

    vhandler el0_32_sync,     handle_bad_exception,  0
    vhandler el0_32_irq,      handle_bad_exception,  0
    vhandler el0_32_fiq,      handle_bad_exception,  0
    vhandler el0_32_serror,   handle_bad_exception,  0
