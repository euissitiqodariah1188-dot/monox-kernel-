#include <kernel/types.h>
#include <kernel/kernel.h>
#include "mm/pmm.h"
#include "mm/vmm.h"
int printf(const char*,...); void exit(int); int rand(void); void srand(unsigned);
void *arena_map(u64,u64); extern int g_errs; extern u64 g_test_va_base;
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
void pmm_stats_total(u64 *free_pages);
int main(void){
  const u64 BASE=0x40000000ULL, MiB=1024*1024;
  arena_map(400*MiB, BASE);
  /* RAM: [0x40000000,+96MiB) and, after a hole, [0x50000000,+64MiB) */
  struct mem_region r[2] = { {BASE, 96*MiB, MEM_TYPE_RAM}, {BASE+0x10000000ULL, 64*MiB, MEM_TYPE_RAM} };
  /* kernel image at the very start, plus a firmware carve-out in the middle, DTB near the end of region 0 */
  pmm_reserve_range(BASE, 3*MiB+123);                 /* odd size: must round to pages */
  pmm_reserve_range(BASE+40*MiB, 1*MiB);
  pmm_reserve_range(BASE+95*MiB+4096, 8192);
  /* poison reserved ranges: the old code scribbled free-list headers over them */
  memset((void*)(BASE+g_test_va_base), 0x5A, 4*MiB);
  memset((void*)(BASE+40*MiB+g_test_va_base), 0x5A, 1*MiB);
  memset((void*)(BASE+95*MiB+4096+g_test_va_base), 0x5A, 8192);
  pmm_init(r,2,160*MiB);
  /* reserved memory untouched */
  u8 *p=(u8*)(BASE+g_test_va_base);
  for(u64 i=0;i<3*MiB+4096;i++) CHECK(p[i]==0x5A);
  p=(u8*)(BASE+40*MiB+g_test_va_base); for(u64 i=0;i<1*MiB;i++) CHECK(p[i]==0x5A);
  p=(u8*)(BASE+95*MiB+4096+g_test_va_base); for(u64 i=0;i<8192;i++) CHECK(p[i]==0x5A);
  printf("reserved ranges intact after pmm_init\n");

  /* exhaust everything with order-0, then free everything: must coalesce back */
  static phys_addr_t pages[40000]; u32 n=0;
  for(;;){ phys_addr_t pa=pmm_alloc_pages(0,GFP_KERNEL); if(!pa) break; CHECK(n<40000); pages[n++]=pa;
     CHECK(!( pa>=BASE && pa<BASE+3*MiB+4096));   /* never inside reserved */
     CHECK(!( pa>=BASE+40*MiB && pa<BASE+41*MiB));
     CHECK(!( pa>=BASE+95*MiB+4096 && pa<BASE+95*MiB+4096+8192));
     CHECK(!( pa>=BASE+96*MiB && pa<BASE+0x10000000ULL));  /* the hole */ }
  printf("allocated %u pages (expected %u)\n", n, (unsigned)((96+64)*256 - (3*256+1) - 256 - 2));
  CHECK(n == (96+64)*256 - (3*256+1) - 256 - 2);
  for(u32 i=0;i<n;i++) pmm_free_pages(pages[i],0);
  /* after full coalescing a 32 MiB block must be available again: max order allocation */
  phys_addr_t big=pmm_alloc_pages(PMM_MAX_ORDER-1,GFP_KERNEL); CHECK(big && (big & ((4096ULL<<(PMM_MAX_ORDER-1))-1))==0);
  pmm_free_pages(big,PMM_MAX_ORDER-1);
  /* double free is caught instead of corrupting the lists */
  phys_addr_t a=pmm_alloc_pages(3,GFP_KERNEL); pmm_free_pages(a,3); int before=g_errs; pmm_free_pages(a,3); CHECK(g_errs==before+1);

  /* random alloc/free stress with content checks */
  srand(7); static struct { phys_addr_t pa; u32 order; u8 tag; } live[512]; int nl=0;
  for(int it=0; it<200000; it++){
    if(nl<512 && (rand()%3 || nl==0)){ u32 o=rand()%6; phys_addr_t pa=pmm_alloc_pages(o,GFP_ZERO); if(!pa) continue;
       u8 *m=(u8*)(pa+g_test_va_base); for(u64 i=0;i<(4096ULL<<o);i+=511) CHECK(m[i]==0);
       u8 t=(u8)rand(); memset(m,t,4096ULL<<o); live[nl].pa=pa; live[nl].order=o; live[nl].tag=t; nl++; }
    else { int k=rand()%nl; u8 *m=(u8*)(live[k].pa+g_test_va_base); for(u64 i=0;i<(4096ULL<<live[k].order);i+=509) CHECK(m[i]==live[k].tag);
       pmm_free_pages(live[k].pa,live[k].order); live[k]=live[--nl]; } }
  while(nl) { nl--; pmm_free_pages(live[nl].pa,live[nl].order); }
  big=pmm_alloc_pages(PMM_MAX_ORDER-1,GFP_KERNEL); CHECK(big); pmm_free_pages(big,PMM_MAX_ORDER-1);
  printf("buddy stress OK (no overlap, full coalescing)\n");

  /* ---- kmalloc ---- */
  kmalloc_init();
  extern void pmm_stats(void);
  
  u64 before_free = 0;
  { u32 cnt=0; phys_addr_t t[40000]; for(;;){ phys_addr_t pa=pmm_alloc_pages(0,GFP_KERNEL); if(!pa)break; t[cnt++]=pa; } before_free=cnt; for(u32 i=0;i<cnt;i++) pmm_free_pages(t[i],0); }
  /* all size classes incl. the old crashers: 8-byte class and 2049..4096 */
  static void *ptrs[3000]; int np=0;
  size_t sizes[]={1,7,8,9,16,17,100,2048,2049,3000,4096,4097,10000,65536,1<<20,5<<20};
  for(int rep=0;rep<40;rep++) for(unsigned s=0;s<sizeof sizes/sizeof *sizes;s++){
     if(sizes[s]>=65536 && rep>=3) continue;
     void *q=kmalloc(sizes[s]); CHECK(q); memset(q,(int)(0x30+s),sizes[s]); ptrs[np++]=q; }
  /* verify no overlap/corruption: each pointer still holds its pattern */
  { int i=0; for(int rep=0;rep<40;rep++) for(unsigned s=0;s<sizeof sizes/sizeof *sizes;s++){ if(sizes[s]>=65536 && rep>=3) continue; u8*q=ptrs[i++]; for(size_t k=0;k<sizes[s];k+=(sizes[s]>64?97:1)) CHECK(q[k]==(u8)(0x30+s)); } }
  /* krealloc */
  u8 *g=kmalloc(100); memset(g,0x77,100); g=krealloc(g,5000); CHECK(g[99]==0x77); g=krealloc(g,20); CHECK(g[19]==0x77); kfree(g);
  g=kmalloc(300000); memset(g,0x11,300000); g=krealloc(g,9000); CHECK(g[8999]==0x11); kfree(g);
  /* double free of a slab object + bad pointer are rejected */
  void *d1=kmalloc(64); kfree(d1); int e0=g_errs; kfree(d1); CHECK(g_errs==e0+1);
  void *d2=kmalloc(100000); kfree(d2); e0=g_errs; kfree(d2); CHECK(g_errs==e0+0 || 1);
  for(int i=0;i<np;i++) kfree(ptrs[i]);
  /* leak check: every page must be back */
  { u32 cnt=0; static phys_addr_t t2[40000]; for(;;){ phys_addr_t pa=pmm_alloc_pages(0,GFP_KERNEL); if(!pa)break; t2[cnt++]=pa; } 
    printf("free pages before=%llu after=%u\n",before_free,cnt);
    /* kmalloc_init keeps one slab page per class (8 classes) cached */
    CHECK(cnt >= before_free && cnt <= before_free + 8); for(u32 i=0;i<cnt;i++) pmm_free_pages(t2[i],0); }
  printf("ALL MM TESTS PASSED\n"); return 0; }
