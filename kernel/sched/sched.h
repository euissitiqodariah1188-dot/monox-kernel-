/* MONOX Kernel - Scheduler Interface */

#ifndef _MONOX_SCHED_H
#define _MONOX_SCHED_H

#include <kernel/types.h>
#include <kernel/kernel.h>

struct task;

void         sched_init(void);
void         sched_cpu_init(u32 cpu);
struct task *task_create(const char *name, void (*entry)(void *), void *arg,
                          u32 prio, size_t stack_sz);
void         schedule(void);
void         task_sleep(u64 ns);
void         task_wake(struct task *t);
void         task_exit(void) __noreturn;
struct task *current_task(void);
pid_t        current_pid(void);
void         task_first_run(void);   /* used by task_trampoline */
void         task_trampoline(void) __attribute__((naked));
void         sched_stats(void);

/* Priority levels */
#define PRIO_REALTIME  0
#define PRIO_HIGH      3
#define PRIO_NORMAL    8
#define PRIO_LOW       12
#define PRIO_IDLE      15

/* CPU count (from cpu.c) */
u32          cpu_count(void);

#endif /* _MONOX_SCHED_H */
