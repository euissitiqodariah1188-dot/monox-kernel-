/* MONOX Kernel - IRQ Interface */

#ifndef _MONOX_IRQ_H
#define _MONOX_IRQ_H

#include <kernel/types.h>

/* pt_regs: saved register frame from vectors.S */
struct pt_regs {
    u64 regs[31];   /* x0 - x30 */
    u64 elr;        /* Exception Link Register */
    u64 spsr;       /* Saved Program Status Register */
};

typedef void (*irq_handler_fn)(u32 irq, void *data);

/* GIC init (addresses from DTB, NOT hardcoded) */
bool gic_is_ready(void);   /* true once gic_init() succeeded */
int  gic_init(phys_addr_t gicd_phys, phys_addr_t gicr_phys, u32 gicr_stride);
int  gic_cpu_init(void);

/* Per-IRQ registration */
int  irq_register(u32 irq, irq_handler_fn fn, void *data);
int  irq_unregister(u32 irq);

/* Called from exception vectors */
void handle_irq(struct pt_regs *regs);
void handle_fiq(struct pt_regs *regs);
void handle_serror(struct pt_regs *regs);
void handle_bad_exception(void);
void handle_sync_exception(struct pt_regs *regs);
void handle_el0_sync(struct pt_regs *regs);

#endif /* _MONOX_IRQ_H */
