#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
void *kmalloc(size_t n){ return malloc(n); }
void *kzalloc(size_t n){ return calloc(1,n); }
void kfree(void*p){ free(p); }
void monox_printk(const char*l,const char*f,...){ va_list a; va_start(a,f); if(getenv("V")){printf("  [%s] ",l); vprintf(f,a);} va_end(a); }
void monox_panic(const char*f,...){ printf("PANIC\n"); abort(); }
unsigned int g_kernel_locks_enabled=0;
void local_irq_disable(void){} unsigned long local_irq_save(void){return 0;} void local_irq_restore(unsigned long f){}
