#include <kernel/types.h>
#include <kernel/kernel.h>
#include "fs/vfs/vfs.h"
int printf(const char*,...); void exit(int);
extern struct fs_ops monoxfs_ops;
void *kmalloc(size_t); void kfree(void*);
#define BS 4096
static u8 disk[64*BS] __attribute__((aligned(8)));
static int bread_(void*p,u64 b,void*buf){ if(b>=64) return -1; memcpy(buf,disk+b*BS,BS); return 0; }
static int bwrite_(void*p,u64 b,const void*buf){ if(b>=64) return -1; memcpy(disk+b*BS,buf,BS); return 0; }
int blk_find_dev(const char*n,int(**br)(void*,u64,void*),int(**bw)(void*,u64,const void*),void**h){ *br=bread_; *bw=bwrite_; *h=0; return 0; }
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
struct sb { u32 magic,version,block_size; u64 nr_blocks,nr_inodes,nr_free_blocks,nr_free_inodes,ib,db,it,ds; u32 isz,ipb; u8 label[64],uuid[16]; u64 mt,wt; u32 flags; u8 role; u8 pad[155]; } __attribute__((packed));
struct ino { u16 mode,uid,gid,nlink; u64 size,c,m,a; u32 direct[12]; u32 ind,dind,tind; u8 ext; u8 pad[155]; } __attribute__((packed));
struct de { u32 ino; u8 type; char name[55]; u8 pad[4]; } __attribute__((packed));
static void put_ino(int n,struct ino*i){ memcpy(disk+3*BS+(n/16)*BS+(n%16)*256,i,256); }
static void mkdisk(void){
  memset(disk,0xAB,sizeof disk);                     /* garbage everywhere: catches uninit reads */
  struct sb s; memset(&s,0,sizeof s);
  s.magic=0x4D4F4E58; s.version=1; s.block_size=BS; s.nr_blocks=64; s.nr_inodes=32; s.nr_free_blocks=64-9; s.nr_free_inodes=28;
  s.ib=1; s.db=2; s.it=3; s.ds=5; s.isz=256; s.ipb=16; memcpy(s.label,"testvol",8);
  memset(disk,0,BS); memcpy(disk,&s,sizeof s);
  memset(disk+1*BS,0,BS); disk[BS]=0x1F;             /* inodes 0..4 used */
  memset(disk+2*BS,0,BS); disk[2*BS]=0x0F;           /* data blocks 0..3 used (blocks 5..8) */
  memset(disk+3*BS,0,2*BS);
  struct ino r; memset(&r,0,sizeof r); r.mode=0755; r.size=3*64; r.direct[0]=5; put_ino(2,&r);
  struct ino h; memset(&h,0,sizeof h); h.mode=0644; h.size=11; h.direct[0]=6; h.nlink=1; put_ino(3,&h);
  struct ino b; memset(&b,0,sizeof b); b.mode=0644; b.size=3*BS; b.direct[0]=7; b.direct[1]=0; b.direct[2]=8; b.nlink=1; put_ino(4,&b);
  struct ino d; memset(&d,0,sizeof d); d.mode=0755; d.size=64; d.direct[0]=9; d.nlink=1; put_ino(5,&d);
  struct de e[3]; memset(e,0,sizeof e);
  e[0].ino=3; e[0].type=0; memcpy(e[0].name,"hello.txt",10);
  e[1].ino=4; e[1].type=0; memcpy(e[1].name,"big",4);
  e[2].ino=5; e[2].type=1; memcpy(e[2].name,"sub",4);
  memset(disk+5*BS,0,BS); memcpy(disk+5*BS,e,sizeof e);
  memset(disk+6*BS,0,BS); memcpy(disk+6*BS,"hello world",11);
  memset(disk+7*BS,'A',BS); memset(disk+8*BS,'C',BS); memset(disk+9*BS,0,BS);
}
int main(void){
  mkdisk();
  CHECK(vfs_init()==MX_OK);
  CHECK(vfs_register_fs("monoxfs",&monoxfs_ops)==MX_OK);
  CHECK(vfs_register_fs("monoxfs",&monoxfs_ops)==MX_EEXIST);       /* no duplicate registration */
  CHECK(vfs_mount("ram","/","monoxfs","rw")==MX_OK);
  CHECK(vfs_mount("ram","/","monoxfs","rw")==MX_EBUSY);

  struct vfs_file *f=vfs_open("/hello.txt",0); CHECK(f);
  char buf[32]; memset(buf,0,sizeof buf);
  CHECK(vfs_read(f,buf,32)==11); CHECK(memcmp(buf,"hello world",11)==0);
  vfs_close(f);
  f=vfs_open("/hello.txt",0); CHECK(f); vfs_close(f);               /* cached path + refcounts */
  for(int i=0;i<50;i++){ f=vfs_open("/hello.txt",0); CHECK(f); vfs_close(f); }  /* old code: UAF after 1st close */

  f=vfs_open("/big",0); CHECK(f);
  static u8 big[3*BS]; CHECK(vfs_read(f,big,sizeof big)==3*BS);
  CHECK(big[0]=='A' && big[BS-1]=='A'); CHECK(big[BS]==0 && big[2*BS-1]==0); CHECK(big[2*BS]=='C');   /* hole reads as zeros */
  vfs_close(f);

  /* partial write into the hole: fresh block must not leak 0xAB garbage */
  f=vfs_open("/big",VFS_WRONLY); CHECK(f);
  CHECK(vfs_seek(f,BS+10,VFS_SEEK_SET)==BS+10);
  CHECK(vfs_write(f,"XYZ",3)==3); vfs_close(f);
  f=vfs_open("/big",0); static u8 chk[BS]; vfs_seek(f,BS,VFS_SEEK_SET); CHECK(vfs_read(f,chk,BS)==BS);
  CHECK(chk[10]=='X'&&chk[12]=='Z'); for(int i=0;i<BS;i++) if(i<10||i>12) CHECK(chk[i]==0);
  vfs_close(f);

  /* append + persistence of bitmaps (vfs_close syncs) */
  f=vfs_open("/hello.txt",VFS_WRONLY|VFS_APPEND); CHECK(f);
  CHECK(vfs_write(f,"!!",2)==2); vfs_close(f);
  CHECK(disk[2*BS]!=0x0F);                                          /* data bitmap changed on disk */
  struct sb *dsb=(struct sb*)disk; CHECK(dsb->nr_free_blocks==64-9-1);
  f=vfs_open("/hello.txt",0); memset(buf,0,32); CHECK(vfs_read(f,buf,32)==13); CHECK(memcmp(buf,"hello world!!",13)==0); vfs_close(f);

  /* directory listing comes from the filesystem */
  struct vfs_dir *d=vfs_opendir("/"); CHECK(d); struct vfs_dirent de; int n=0;
  while(vfs_readdir(d,&de)==MX_OK) n++;
  CHECK(n==3); vfs_closedir(d);

  struct vfs_stat st; CHECK(vfs_stat("/sub",&st)==MX_OK && st.type==INODE_DIR);
  CHECK(vfs_stat("/nope",&st)==MX_ENOENT);
  CHECK(vfs_open("",VFS_CREAT)==NULL);                              /* old: write before buffer */
  CHECK(vfs_open("rel",VFS_CREAT)==NULL);
  CHECK(vfs_mkdir("",0755)!=MX_OK);

  /* busy unmount, then clean unmount frees everything */
  f=vfs_open("/hello.txt",0); CHECK(vfs_unmount("/")==MX_EBUSY); vfs_close(f);
  CHECK(vfs_unmount("/")==MX_OK);
  CHECK(vfs_open("/hello.txt",0)==NULL);

  /* read-only mount refuses writes and does not flush */
  mkdisk(); u8 snap[64*BS]; memcpy(snap,disk,sizeof snap);
  CHECK(vfs_mount("ram","/","monoxfs","ro")==MX_OK);
  f=vfs_open("/hello.txt",VFS_RDWR); CHECK(f); CHECK(vfs_write(f,"zz",2)==MX_EACCES); vfs_close(f);
  CHECK(vfs_unmount("/")==MX_OK); CHECK(memcmp(snap,disk,sizeof snap)==0);

  /* corrupt superblock geometry is rejected (inode_size==0 used to divide by zero) */
  mkdisk(); ((struct sb*)disk)->isz=0; CHECK(vfs_mount("ram","/","monoxfs","rw")==MX_EIO);
  mkdisk(); ((struct sb*)disk)->ds=1000; CHECK(vfs_mount("ram","/","monoxfs","rw")==MX_EIO);

  printf("ALL FS TESTS PASSED\n"); return 0; }
