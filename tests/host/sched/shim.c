#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
typedef unsigned long long u64; typedef unsigned int u32;
unsigned int g_kernel_locks_enabled=0;
int freed_stacks=0, alloc_stacks=0, freed_tasks=0, live_alloc=0;
unsigned long long stack_log[256]; int stack_n=0, free_log_n=0; unsigned long long free_log[256];
void *kzalloc(size_t n){ live_alloc++; return calloc(1,n);} void *kmalloc(size_t n){ live_alloc++; return malloc(n);} void kfree(void*p){ if(p){ live_alloc--; freed_tasks++; free(p);} }
void monox_printk(const char*l,const char*f,...){} void monox_panic(const char*f,...){ printf("PANIC\n"); abort(); }
void local_irq_disable(void){} unsigned long local_irq_save(void){return 0;} void local_irq_restore(unsigned long x){} void local_irq_enable(void){}
void cpu_wfi(void){} unsigned cpu_count(void){return 1;}
u64 vmm_alloc_pages(u32 order,u32 fl){ void*p=aligned_alloc(4096,4096UL<<order); memset(p,0,4096UL<<order); alloc_stacks++; stack_log[stack_n++]=(u64)p; return (u64)p; }
void vmm_free_region(u64 va){ free_log[free_log_n++]=va; freed_stacks++; free((void*)va); }
void timer_oneshot(u64 ns,void(*cb)(void*),void*a){ cb(a); }
/* ucontext based __sched_switch: contexts keyed by &task->ctx */
struct cpu_ctx { u64 x19,x20,x21,x22,x23,x24,x25,x26,x27,x28,fp,lr,sp; };
static struct { struct cpu_ctx*key; ucontext_t uc; int made; } tab[64]; static int ntab=0;
extern void task_first_run(void); extern void task_exit(void);
static void host_tramp(void){ struct cpu_ctx*c=NULL; for(int i=0;i<ntab;i++) if(tab[i].uc.uc_stack.ss_sp && 0){} extern struct cpu_ctx *g_cur_ctx; c=g_cur_ctx; void(*e)(void*)=(void(*)(void*))c->x19; void*arg=(void*)c->x20; task_first_run(); e(arg); task_exit(); }
struct cpu_ctx *g_cur_ctx;
static int find(struct cpu_ctx*k){ for(int i=0;i<ntab;i++) if(tab[i].key==k) return i; tab[ntab].key=k; tab[ntab].made=0; return ntab++; }
void __sched_switch(struct cpu_ctx*prev,struct cpu_ctx*next){
  int ip=find(prev); int in=find(next);
  if(!tab[in].made){ tab[in].made=1; getcontext(&tab[in].uc); tab[in].uc.uc_stack.ss_sp=malloc(256*1024); tab[in].uc.uc_stack.ss_size=256*1024; tab[in].uc.uc_link=NULL; g_cur_ctx=next; makecontext(&tab[in].uc,host_tramp,0); }
  tab[ip].made=1;
  swapcontext(&tab[ip].uc,&tab[in].uc);
}
