#include <kernel/types.h>
#include <kernel/kernel.h>
#include "kernel/dtb.h"
int printf(const char*,...); int vprintf(const char*, __builtin_va_list); void exit(int);
void *fopen(const char*,const char*); u64 fread(void*,u64,u64,void*);
void monox_printk(const char*l,const char*f,...){__builtin_va_list a;__builtin_va_start(a,f);printf("  [%s] ",l);vprintf(f,a);__builtin_va_end(a);}
void *phys_to_virt_early(phys_addr_t pa){return (void*)pa;}
void monox_panic(const char*f,...){exit(9);}
volatile u32 g_kernel_locks_enabled=0;
void local_irq_disable(void){} u64 local_irq_save(void){return 0;} void local_irq_restore(u64 f){}
extern void dtb_parse(phys_addr_t, struct board_info*);
int main(){ void*f=fopen("t.dtb","rb"); static unsigned char buf[8192] __attribute__((aligned(8))); u64 n=fread(buf,1,sizeof buf,f);
 static struct board_info b; dtb_parse((phys_addr_t)buf,&b);
 printf("model=%s ram=%llu regions=%u\n",b.model,(unsigned long long)b.total_ram,b.nr_regions);
 for(u32 i=0;i<b.nr_regions;i++)printf("  region[%u] base=0x%llx size=0x%llx type=%d\n",i,(unsigned long long)b.regions[i].base,(unsigned long long)b.regions[i].size,b.regions[i].type);
 printf("uart=0x%llx gicd=0x%llx gicr=0x%llx stride=0x%x\ngpu=0x%llx/0x%llx irq=%u compat=%s\n",(unsigned long long)b.uart_base,(unsigned long long)b.gic_dist_base,(unsigned long long)b.gic_redist_base,b.gic_redist_stride,(unsigned long long)b.gpu_mmio,(unsigned long long)b.gpu_mmio_size,b.gpu_irq,b.gpu_compatible);
 for(int i=0;b.ts_dtb_props[i];i+=2)printf("  ts %s = %s\n",b.ts_dtb_props[i],b.ts_dtb_props[i+1]);
 printf("psci_hvc=%d\n",(int)b.psci_hvc); printf("dtb_size=%llu (file %llu)\n",(unsigned long long)b.dtb_size,(unsigned long long)n); return 0;}
