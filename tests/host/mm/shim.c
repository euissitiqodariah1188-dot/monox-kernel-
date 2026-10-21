#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/mman.h>
typedef unsigned long long u64;
u64 g_test_va_base; unsigned int g_kernel_locks_enabled=0;
int g_errs=0;
void monox_printk(const char*l,const char*f,...){ va_list a; va_start(a,f); if(l[0]<='3'){ if(getenv("V")||1){ printf("  [%s] ",l); vprintf(f,a);} if(strstr(f,"DOUBLE")||strstr(f,"bad")) g_errs++; } va_end(a); }
void monox_panic(const char*f,...){ printf("PANIC\n"); abort(); }
void local_irq_disable(void){} unsigned long local_irq_save(void){return 0;} void local_irq_restore(unsigned long x){}
void mmu_pmm_ready(void){}
void *phys_to_virt_early(unsigned long long pa){ return (void*)(pa+g_test_va_base); }
void *arena_map(unsigned long long size, unsigned long long pa_base){
   void *a=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
   g_test_va_base = (u64)a - pa_base; return a; }
