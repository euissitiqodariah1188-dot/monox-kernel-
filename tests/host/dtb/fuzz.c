#include <kernel/types.h>
#include <kernel/kernel.h>
#include "kernel/dtb.h"
void *fopen(const char*,const char*); u64 fread(void*,u64,u64,void*); int posix_memalign(void**,u64,u64); int rand(void); void free(void*); void srand(unsigned); int printf(const char*,...);
void monox_printk(const char*l,const char*f,...){}
void *phys_to_virt_early(phys_addr_t pa){return (void*)pa;}
void monox_panic(const char*f,...){}
volatile u32 g_kernel_locks_enabled=0;
void local_irq_disable(void){} u64 local_irq_save(void){return 0;} void local_irq_restore(u64 f){}
extern void dtb_parse(phys_addr_t, struct board_info*);
int main(){ static unsigned char orig[8192]; void*f=fopen("t.dtb","rb"); u64 n=fread(orig,1,sizeof orig,f); srand(1);
 static struct board_info b; int runs=0;
 for(int it=0; it<20000; it++){
   u64 len = (it%3==0) ? (u64)(rand()%n) : n; if(len<8) len=8;      /* truncation */
   void*p; posix_memalign(&p,8,len?len:8); __builtin_memcpy(p,orig,len);
   if(it%3!=0){ int k=1+rand()%6; while(k--) ((unsigned char*)p)[rand()%len]=(unsigned char)rand(); }
   if(len>=8){ unsigned char*q=p; q[4]=len>>24;q[5]=len>>16;q[6]=len>>8;q[7]=len; }
   __builtin_memset(&b,0,sizeof b); dtb_parse((phys_addr_t)p,&b); runs++; free(p);
 } printf("fuzz runs=%d, no ASAN errors\n",runs); return 0; }
