#include <kernel/types.h>
#include <kernel/kernel.h>
#include "kernel/sched/sched.c"
int printf(const char*,...); void exit(int);
extern int freed_stacks, alloc_stacks, live_alloc; extern unsigned long long free_log[], stack_log[]; extern int free_log_n, stack_n;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
static char logbuf[256]; static int ln=0;
static void worker(void *arg){ char id=(char)(long)arg; for(int i=0;i<3;i++){ logbuf[ln++]=id; schedule(); } }
static void sleeper(void *arg){ logbuf[ln++]='s'; task_sleep(1000000); logbuf[ln++]='S'; }
static void quick(void *arg){ logbuf[ln++]='q'; }
static void lone(void *arg){ for(int i=0;i<5;i++){ logbuf[ln++]='L'; schedule(); } }
int main(void){
  sched_init(); sched_cpu_init(0); sched_cpu_init(0);          /* second init must not leak/replace idle */
  CHECK(current_pid()==0);
  struct task *a=task_create("A",worker,(void*)'A',PRIO_NORMAL,65536);
  struct task *b=task_create("B",worker,(void*)'B',PRIO_NORMAL,65536);
  CHECK(a&&b); CHECK(a->pid==1 && b->pid==2);                 /* PIDs start at 1 now */
  CHECK(a->stack_size==65536);                                /* 64 KiB request -> order 4, not order 16 */
  struct task *c=task_create("s",sleeper,NULL,PRIO_NORMAL,4096);
  struct task *d=task_create("q",quick,NULL,PRIO_NORMAL,8192);
  CHECK(c&&d && c->stack_size==4096 && d->stack_size==8192);
  for(int i=0;i<200;i++){ schedule(); }
  logbuf[ln]=0; printf("log: %s\n",logbuf);
  /* every task ran to completion */
  int cnt[256]={0}; for(int i=0;i<ln;i++) cnt[(unsigned char)logbuf[i]]++;
  CHECK(cnt['A']==3 && cnt['B']==3 && cnt['s']==1 && cnt['S']==1 && cnt['q']==1);
  /* round robin interleaving: A and B alternate */
  CHECK(logbuf[0]=='A' && logbuf[1]=='B');
  /* all four exited tasks were reaped: stack freed exactly once each, with the right address */
  CHECK(freed_stacks==4 && alloc_stacks==4);
  for(int i=0;i<4;i++){ int ok=0; for(int j=0;j<free_log_n;j++) if(free_log[j]==stack_log[i]) ok++; CHECK(ok==1); }
  /* only the 2 idle... (1 idle task) remain allocated: all task structs were freed */
  CHECK(live_alloc==1);
  /* a lone runnable task keeps the CPU instead of bouncing through idle forever */
  ln=0; task_create("L",lone,NULL,PRIO_NORMAL,8192);
  for(int i=0;i<50;i++) schedule();
  logbuf[ln]=0; CHECK(ln==5); CHECK(live_alloc==1 && freed_stacks==5);
  printf("ALL SCHED TESTS PASSED\n"); return 0; }
