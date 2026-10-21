/* MONOX Kernel - Preemptive Scheduler
 *
 * Design: Multi-level feedback queue (MLFQ) with per-CPU run queues.
 * Priority levels: 0 (real-time) .. 15 (idle)
 * Default: priority 8 (normal)
 * Gaming/interactive tasks boost automatically based on recent sleep ratio.
 * Completely lock-free per-CPU scheduling; inter-CPU migration uses IPI.
 *
 * Bug-fix pass:
 *  - task_exit() freed the task's own kernel stack (and zeroed it!) while
 *    still running on it, then kfree()'d the task struct that rq->current
 *    still pointed to, then switched *from* freed memory => guaranteed
 *    crash on the first task that returned. Exiting tasks are now reaped by
 *    the NEXT task after the context switch has completed.
 *  - task_create() passed a page COUNT where vmm_alloc_pages() expects an
 *    ORDER: the 64 KiB init stack asked for order 16 (256 MiB) and failed,
 *    so PID 1 could never start.
 *  - task_wake() enqueued the CURRENT task a second time (task_sleep with a
 *    timer that fires immediately), corrupting the run-queue list.
 *  - schedule() re-enabled IRQs before the switch (a tick could re-enter
 *    schedule() mid-switch) and never let a lone runnable task keep the CPU.
 *  - __sched_switch used extended asm whose inputs the compiler may place in
 *    x19..x28 - registers the asm itself overwrites. It is now a real
 *    assembly routine.
 *  - PIDs started at 2, so "PID 1" (monox_init, trusted by SATPAM) never
 *    existed.
 *  - Locks are IRQ-safe, run-queue index is bounded.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "sched.h"
#include "../mm/vmm.h"
#include "../mm/pmm.h"
#include "../arch/arm64/cpu/cpu.h"

/* -------------------------------------------------------
 * Constants
 * -------------------------------------------------------*/
#define SCHED_NR_PRIO     16          /* priority levels               */
#define SCHED_TIMESLICE   4000000ULL  /* 4 ms in nanoseconds           */
#define SCHED_MIN_GRAN    500000ULL   /* 0.5 ms minimum slice          */
#define SCHED_BOOST_SLEEP 2000000ULL  /* task slept > 2 ms → boost     */
#define SCHED_MAX_TASKS   4096        /* per system, tunable           */

/* -------------------------------------------------------
 * Task state
 * -------------------------------------------------------*/
typedef enum {
    TASK_RUNNING    = 0,  /* on a run queue or currently executing */
    TASK_SLEEPING   = 1,  /* blocked on event / wait queue         */
    TASK_STOPPED    = 2,  /* SIGSTOP / ptrace                      */
    TASK_ZOMBIE     = 3,  /* exited, waiting for parent to collect */
    TASK_DEAD       = 4,  /* fully cleaned up                      */
} task_state_t;

/* -------------------------------------------------------
 * CPU context (saved on context switch)
 * -------------------------------------------------------*/
struct cpu_ctx {
    u64 x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    u64 fp;    /* x29 */
    u64 lr;    /* x30 - return address */
    u64 sp;    /* stack pointer */
};

/* -------------------------------------------------------
 * Task descriptor
 * -------------------------------------------------------*/
struct task {
    /* Identity */
    pid_t           pid;
    pid_t           ppid;
    uid_t           uid;
    gid_t           gid;
    char            name[64];

    /* Scheduling */
    u32             prio;            /* 0=highest 15=lowest           */
    u32             static_prio;     /* fixed priority                */
    task_state_t    state;
    u64             runtime_ns;      /* total CPU time used           */
    u64             vruntime;        /* virtual runtime (MLFQ slot)   */
    u64             sleep_start;     /* when task went to sleep       */
    u64             last_run;        /* timestamp of last schedule    */
    u64             timeslice;       /* remaining timeslice ns        */
    s32             boost;           /* temporary prio boost          */
    u32             cpu;             /* assigned CPU                  */

    /* Memory */
    phys_addr_t     pgd;             /* page global directory PA      */
    u64             stack_top;       /* kernel stack VA               */
    u64             ustack_top;      /* user stack VA                 */
    size_t          stack_size;

    /* CPU context (callee-saved regs + SP/LR) */
    struct cpu_ctx  ctx;

    /* Links */
    struct list_head rq_node;        /* run queue list                */
    struct list_head task_list;      /* global task list              */
    struct list_head wait_node;      /* wait queue                    */

    /* Exit */
    s32             exit_code;
};

/* -------------------------------------------------------
 * Per-CPU run queue
 * -------------------------------------------------------*/
struct rq {
    spinlock_t       lock;
    struct list_head queues[SCHED_NR_PRIO];  /* MLFQ levels          */
    u32              nr_running;
    struct task     *current;
    struct task     *idle;
    u64              clock;           /* monotonic clock ns            */
    u64              switch_count;
    struct task     *dead;            /* exited task to reap after the switch */
};

static struct rq g_rq[MONOX_MAX_CPUS];

/* -------------------------------------------------------
 * Global task table
 * -------------------------------------------------------*/
static struct task  *g_task_table[SCHED_MAX_TASKS];
static atomic_t      g_pid_counter = ATOMIC_INIT(0);   /* first task => PID 1 */
static spinlock_t    g_task_lock   = SPIN_LOCK_INIT;
static LIST_HEAD(g_task_list);

/* -------------------------------------------------------
 * Timer: read hardware counter (ARM64 CNTVCT_EL0)
 * -------------------------------------------------------*/
static u64 sched_clock(void)
{
    u64 cnt, freq;
    __asm__ volatile("mrs %0, cntvct_el0"  : "=r"(cnt));
    __asm__ volatile("mrs %0, cntfrq_el0"  : "=r"(freq));
    if (!freq) return cnt;
    /* ns = cnt * 1e9 / freq; use 64-bit arithmetic */
    return (cnt / freq) * 1000000000ULL + (cnt % freq) * 1000000000ULL / freq;
}

/* -------------------------------------------------------
 * sched_init - called once on primary CPU
 * -------------------------------------------------------*/
void sched_init(void)
{
    for (u32 cpu = 0; cpu < MONOX_MAX_CPUS; cpu++) {
        spin_lock_init(&g_rq[cpu].lock);
        for (u32 i = 0; i < SCHED_NR_PRIO; i++)
            list_init(&g_rq[cpu].queues[i]);
        g_rq[cpu].nr_running  = 0;
        g_rq[cpu].current     = NULL;
        g_rq[cpu].idle        = NULL;
        g_rq[cpu].clock       = 0;
        g_rq[cpu].switch_count= 0;
        g_rq[cpu].dead        = NULL;
    }

    pr_info("SCHED: %u-level MLFQ ready for %u CPUs\n",
            SCHED_NR_PRIO, MONOX_MAX_CPUS);
}

/* -------------------------------------------------------
 * sched_cpu_init - per-CPU init; creates idle task
 * -------------------------------------------------------*/
void sched_cpu_init(u32 cpu)
{
    if (cpu >= MONOX_MAX_CPUS || g_rq[cpu].idle)    /* bad id / already done */
        return;

    struct task *idle = kzalloc(sizeof(*idle));
    BUG_ON(!idle);

    idle->pid         = 0;
    idle->ppid        = 0;
    idle->prio        = SCHED_NR_PRIO - 1;  /* lowest */
    idle->static_prio = SCHED_NR_PRIO - 1;
    idle->state       = TASK_RUNNING;
    idle->cpu         = cpu;
    idle->timeslice   = SCHED_TIMESLICE;
    strncpy(idle->name, "idle", sizeof(idle->name) - 1);
    list_init(&idle->rq_node);
    list_init(&idle->task_list);
    list_init(&idle->wait_node);

    g_rq[cpu].idle    = idle;
    g_rq[cpu].current = idle;
}

/* -------------------------------------------------------
 * Queue helpers (caller holds rq->lock)
 * -------------------------------------------------------*/
static __always_inline bool task_queued(const struct task *t)
{
    return t->rq_node.next && t->rq_node.next != &t->rq_node;
}

static void rq_enqueue(struct rq *rq, struct task *t, bool tail)
{
    if (tail) list_add_tail(&t->rq_node, &rq->queues[t->prio]);
    else      list_add(&t->rq_node, &rq->queues[t->prio]);
    rq->nr_running++;
}

static void rq_dequeue(struct rq *rq, struct task *t)
{
    list_del_init(&t->rq_node);
    rq->nr_running--;
}

/* -------------------------------------------------------
 * task_create - create and enqueue a new task
 * -------------------------------------------------------*/
struct task *task_create(const char *name, void (*entry)(void *), void *arg,
                          u32 prio, size_t stack_sz)
{
    if (!entry) return NULL;
    if (prio >= SCHED_NR_PRIO) prio = 8;
    if (stack_sz < 4096) stack_sz = 4096;

    /* vmm_alloc_pages() takes an ORDER (2^order pages), not a page count.
     * Round the requested size up to a power of two pages. */
    u32 order = 0;
    while (((size_t)4096 << order) < stack_sz && order < PMM_MAX_ORDER - 1)
        order++;
    stack_sz = (size_t)4096 << order;

    struct task *t = kzalloc(sizeof(*t));
    if (!t) return NULL;

    /* Allocate kernel stack */
    u64 stack_va = vmm_alloc_pages(order, VM_READ | VM_WRITE);
    if (!stack_va) { kfree(t); return NULL; }

    t->pid         = (pid_t)atomic_inc_return(&g_pid_counter);
    t->ppid        = current_task() ? current_task()->pid : 0;
    t->prio        = prio;
    t->static_prio = prio;
    t->state       = TASK_RUNNING;
    t->stack_top   = stack_va + stack_sz;
    t->stack_size  = stack_sz;
    t->timeslice   = SCHED_TIMESLICE;
    t->cpu         = smp_processor_id(); /* place on current CPU */
    if (name) strncpy(t->name, name, sizeof(t->name) - 1);

    list_init(&t->rq_node);
    list_init(&t->task_list);
    list_init(&t->wait_node);

    /*
     * Set up initial CPU context so when __sched_switch first
     * executes this task, it returns into task_trampoline.
     */
    t->ctx.sp = t->stack_top - 16;
    t->ctx.fp = 0;
    t->ctx.lr = (u64)task_trampoline;
    /* Pass entry/arg via callee-saved regs x19/x20 */
    t->ctx.x19 = (u64)entry;
    t->ctx.x20 = (u64)arg;

    /* Add to global task list */
    u64 f;
    spin_lock_irqsave(&g_task_lock, f);
    if (t->pid < SCHED_MAX_TASKS)
        g_task_table[t->pid] = t;
    list_add_tail(&t->task_list, &g_task_list);
    spin_unlock_irqrestore(&g_task_lock, f);

    /* Enqueue on target CPU run queue */
    struct rq *rq = &g_rq[t->cpu];
    spin_lock_irqsave(&rq->lock, f);
    rq_enqueue(rq, t, true);
    spin_unlock_irqrestore(&rq->lock, f);

    return t;
}

/* -------------------------------------------------------
 * __sched_switch - low-level context switch.
 *   x0 = &prev->ctx, x1 = &next->ctx
 * A top-level assembly routine (not inline asm) so the compiler cannot
 * allocate its operands in the callee-saved registers it rewrites.
 * struct cpu_ctx layout: x19..x28 (0..72), fp (80), lr (88), sp (96).
 * -------------------------------------------------------*/
extern void __sched_switch(struct cpu_ctx *prev, struct cpu_ctx *next);

__asm__(
    ".text                              \n"
    ".balign 16                         \n"
    ".global __sched_switch             \n"
    ".type   __sched_switch, %function  \n"
    "__sched_switch:                    \n"
    "   stp  x19, x20, [x0, #( 0*8)]    \n"
    "   stp  x21, x22, [x0, #( 2*8)]    \n"
    "   stp  x23, x24, [x0, #( 4*8)]    \n"
    "   stp  x25, x26, [x0, #( 6*8)]    \n"
    "   stp  x27, x28, [x0, #( 8*8)]    \n"
    "   stp  x29, x30, [x0, #(10*8)]    \n"
    "   mov  x9,  sp                    \n"
    "   str  x9,  [x0, #(12*8)]         \n"
    "   ldp  x19, x20, [x1, #( 0*8)]    \n"
    "   ldp  x21, x22, [x1, #( 2*8)]    \n"
    "   ldp  x23, x24, [x1, #( 4*8)]    \n"
    "   ldp  x25, x26, [x1, #( 6*8)]    \n"
    "   ldp  x27, x28, [x1, #( 8*8)]    \n"
    "   ldp  x29, x30, [x1, #(10*8)]    \n"
    "   ldr  x9,  [x1, #(12*8)]         \n"
    "   mov  sp,  x9                    \n"
    "   ret                             \n"
    ".size __sched_switch, .-__sched_switch \n"
);

/* -------------------------------------------------------
 * Reap an exited task. Only ever called from a DIFFERENT task's context,
 * after the switch away from 'z' completed, so its stack is no longer in
 * use.
 * -------------------------------------------------------*/
static void task_reap(struct task *z)
{
    if (z->stack_top)
        vmm_free_region(z->stack_top - z->stack_size);
    z->state = TASK_DEAD;
    kfree(z);
}

static void finish_switch(void)
{
    struct rq *rq = &g_rq[smp_processor_id()];
    struct task *z = rq->dead;
    if (z) {
        rq->dead = NULL;
        task_reap(z);
    }
}

/* Called by task_trampoline: a brand-new task has no schedule() frame to
 * return into, so it must finish the switch and enable IRQs itself. */
void task_first_run(void)
{
    finish_switch();
    local_irq_enable();
}

/* -------------------------------------------------------
 * task_trampoline - entry wrapper for new tasks
 *   x19 = function pointer, x20 = argument
 * -------------------------------------------------------*/
void __attribute__((naked)) task_trampoline(void)
{
    __asm__ volatile(
        "bl   task_first_run \n"  /* reap previous task, enable IRQs */
        "mov  x0, x20        \n"  /* arg -> first parameter */
        "blr  x19            \n"  /* call entry(arg)        */
        "bl   task_exit      \n"  /* clean exit             */
        "b    .              \n"  /* task_exit never returns */
    );
}

/* -------------------------------------------------------
 * pick_next - select next task to run on this CPU
 * -------------------------------------------------------*/
static struct task *pick_next(struct rq *rq)
{
    for (u32 p = 0; p < SCHED_NR_PRIO; p++) {
        if (!list_empty(&rq->queues[p])) {
            return list_entry(rq->queues[p].next, struct task, rq_node);
        }
    }
    return rq->idle;
}

/* -------------------------------------------------------
 * schedule - called to yield or preempt
 * IRQs stay disabled across the switch; each context restores its own
 * saved flags when it resumes.
 * -------------------------------------------------------*/
void schedule(void)
{
    struct rq *rq = &g_rq[smp_processor_id()];
    u64 flags = local_irq_save();
    spin_lock(&rq->lock);

    struct task *prev = rq->current;
    if (!prev || !rq->idle) {            /* scheduler not initialised yet */
        spin_unlock(&rq->lock);
        local_irq_restore(flags);
        return;
    }

    /* Put prev back first so a lone runnable task keeps running instead of
     * bouncing through idle. */
    if (prev != rq->idle && prev->state == TASK_RUNNING) {
        /* Boost: if ran for full timeslice, demote priority slightly */
        if (prev->timeslice == 0) {
            prev->timeslice = SCHED_TIMESLICE;
            if (prev->prio < SCHED_NR_PRIO - 2)
                prev->prio++;
        }
        if (!task_queued(prev))
            rq_enqueue(rq, prev, true);
    }

    struct task *next = pick_next(rq);
    if (next != rq->idle)
        rq_dequeue(rq, next);

    if (next == prev) {
        spin_unlock(&rq->lock);
        local_irq_restore(flags);
        return;
    }

    /* An exited task is released by whoever runs next */
    if (prev != rq->idle && prev->state == TASK_ZOMBIE)
        rq->dead = prev;

    rq->current = next;
    rq->switch_count++;

    u64 now = sched_clock();
    next->last_run = now;

    /* Interactive boost: if task was sleeping, restore static priority */
    if (next->sleep_start > 0) {
        u64 slept = now - next->sleep_start;
        if (slept >= SCHED_BOOST_SLEEP && next->prio > next->static_prio) {
            next->prio = next->static_prio;
        }
        next->sleep_start = 0;
    }
    next->timeslice = SCHED_TIMESLICE;

    spin_unlock(&rq->lock);

    /* Switch context (IRQs still masked). When prev is scheduled again we
     * continue right here. */
    __sched_switch(&prev->ctx, &next->ctx);

    finish_switch();
    local_irq_restore(flags);
}

/* -------------------------------------------------------
 * task_sleep - move current task to sleeping state
 * -------------------------------------------------------*/
void task_sleep(u64 ns)
{
    struct rq *rq = &g_rq[smp_processor_id()];

    u64 flags;
    spin_lock_irqsave(&rq->lock, flags);

    struct task *t = rq->current;
    if (!t || t == rq->idle) {           /* the idle thread never sleeps */
        spin_unlock_irqrestore(&rq->lock, flags);
        return;
    }
    t->state       = TASK_SLEEPING;
    t->sleep_start = sched_clock();

    spin_unlock_irqrestore(&rq->lock, flags);

    /* Arm a timer to wake us (timer driver calls task_wake) */
    extern void timer_oneshot(u64 ns, void (*cb)(void *), void *arg);
    timer_oneshot(ns, (void (*)(void *))task_wake, t);

    schedule();
}

/* -------------------------------------------------------
 * task_wake - wake a sleeping task
 * -------------------------------------------------------*/
void task_wake(struct task *t)
{
    if (!t) return;
    struct rq *rq = &g_rq[t->cpu & (MONOX_MAX_CPUS - 1)];

    u64 flags;
    spin_lock_irqsave(&rq->lock, flags);

    if (t->state == TASK_SLEEPING) {
        t->state = TASK_RUNNING;
        /* The task may still be executing (woken before it reached
         * schedule()): then schedule() re-queues it itself. Adding it
         * here too linked the same node twice. */
        if (t != rq->current && !task_queued(t))
            rq_enqueue(rq, t, false);
    }

    spin_unlock_irqrestore(&rq->lock, flags);
}

/* -------------------------------------------------------
 * task_exit - clean up and remove current task
 * The stack and the task struct are freed by the next task (see
 * finish_switch); freeing them here would free memory we are using.
 * -------------------------------------------------------*/
void task_exit(void)
{
    struct rq *rq = &g_rq[smp_processor_id()];

    u64 flags;
    spin_lock_irqsave(&rq->lock, flags);
    struct task *t = rq->current;
    BUG_ON(!t || t == rq->idle);
    t->state = TASK_ZOMBIE;
    spin_unlock_irqrestore(&rq->lock, flags);

    spin_lock_irqsave(&g_task_lock, flags);
    list_del_init(&t->task_list);
    if (t->pid < SCHED_MAX_TASKS && g_task_table[t->pid] == t)
        g_task_table[t->pid] = NULL;
    spin_unlock_irqrestore(&g_task_lock, flags);

    schedule();                          /* never returns to this task */
    BUG();
    for (;;) cpu_wfi();
}

/* -------------------------------------------------------
 * current_task / current_pid
 * -------------------------------------------------------*/
struct task *current_task(void)
{
    return g_rq[smp_processor_id()].current;
}

pid_t current_pid(void)
{
    struct task *t = current_task();
    return t ? t->pid : 0;
}

/* -------------------------------------------------------
 * sched_stats
 * -------------------------------------------------------*/
void sched_stats(void)
{
    u32 n = cpu_count();
    if (n > MONOX_MAX_CPUS) n = MONOX_MAX_CPUS;
    for (u32 cpu = 0; cpu < n; cpu++) {
        pr_info("SCHED CPU%u: running=%u switches=%llu\n",
                cpu, g_rq[cpu].nr_running, g_rq[cpu].switch_count);
    }
}
