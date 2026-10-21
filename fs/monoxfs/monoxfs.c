/* MONOX Kernel - MonoxFS Native Filesystem
 *
 * Layout on disk:
 *  Block 0       : Superblock
 *  Block 1..N    : Inode bitmap
 *  Block N+1..M  : Data bitmap
 *  Block M+1..K  : Inode table
 *  Block K+1..end: Data blocks
 *
 * Block size: 4096 bytes
 * Max file size: direct + single/double indirect = ~1 GiB
 * Supports:
 *   .Runap  - app bundle (executable)
 *   .Config - configuration file
 *   Standard: dir, regular file, symlink
 *
 * Partition layout (as required):
 *   Partition 0 → SYSTEM (kernel, OS): mounted at /
 *   Partition 1 → USER   (user data) : mounted at /user
 *
 * Bug-fix pass:
 *  - mount read a whole 4096-byte block straight into the ~340-byte
 *    in-memory superblock: a 3.7 KiB heap overflow (past the end of the slab
 *    page) on every mount.
 *  - struct monoxfs_dirent was 60 bytes while the format (and the readdir
 *    stride) is 64; struct monoxfs_inode was 228 bytes instead of 256.
 *  - MX_ENOTDIR was undefined (compile error); an extern dentry_alloc()
 *    declaration pointed at a static function.
 *  - bitmaps/superblock were never written back => block/inode allocation
 *    state was lost at every reboot (blocks handed out twice).
 *  - the "ro" mount option was ignored: the system partition was writable.
 *  - reads stopped at the first sparse hole instead of returning zeros;
 *    partial writes to fresh blocks leaked old disk contents; read errors
 *    in the indirect-block path used uninitialised stack data.
 *  - the superblock was not validated (inode_size == 0 => division by zero,
 *    absurd geometry => out-of-range block access).
 *  - nothing ever freed a mount (no unmount): bitmaps and private data leaked.
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "../../fs/vfs/vfs.h"
#include "../../mm/vmm.h"
#include "monoxfs.h"

/* -------------------------------------------------------
 * On-disk superblock (sits at block 0)
 * -------------------------------------------------------*/
#define MONOXFS_MAGIC    0x4D4F4E58  /* "MONX" */
#define MONOXFS_VERSION  1
#define MONOXFS_BLOCK_SZ 4096

struct __packed monoxfs_super {
    u32 magic;
    u32 version;
    u32 block_size;
    u64 nr_blocks;
    u64 nr_inodes;
    u64 nr_free_blocks;
    u64 nr_free_inodes;
    u64 inode_bitmap_start;  /* block index */
    u64 data_bitmap_start;
    u64 inode_table_start;
    u64 data_start;
    u32 inode_size;          /* sizeof(monoxfs_inode) = 256 */
    u32 inodes_per_block;    /* 4096 / 256 = 16 */
    u8  label[64];           /* volume label */
    u8  uuid[16];
    u64 mount_time;
    u64 write_time;
    u32 flags;               /* MXFS_FLAG_* */
    u8  partition_role;      /* 0=SYSTEM 1=USER */
    u8  _pad[155];
};
_Static_assert(sizeof(struct monoxfs_super) <= MONOXFS_BLOCK_SZ, "superblock must fit in a block");

#define MXFS_FLAG_CLEAN   (1 << 0)
#define MXFS_FLAG_ERRORS  (1 << 1)

/* -------------------------------------------------------
 * On-disk inode (256 bytes)
 * -------------------------------------------------------*/
#define MONOXFS_N_DIRECT  12
#define MONOXFS_INODE_SZ  256

struct __packed monoxfs_inode {
    u16  mode;              /* type + permissions */
    u16  uid;
    u16  gid;
    u16  nlink;
    u64  size;
    u64  ctime;
    u64  mtime;
    u64  atime;
    u32  direct[MONOXFS_N_DIRECT]; /* direct block pointers */
    u32  indirect;          /* single indirect */
    u32  dindirect;         /* double indirect  */
    u32  tindirect;         /* triple indirect  */
    u8   file_type_ext;     /* MXFS_EXT_* */
    u8   _pad[155];         /* 101 bytes above + 155 = 256 */
};
_Static_assert(sizeof(struct monoxfs_inode) == MONOXFS_INODE_SZ, "inode must be 256 bytes");

#define MXFS_EXT_REGULAR  0
#define MXFS_EXT_RUNAP    1   /* .Runap application bundle */
#define MXFS_EXT_CONFIG   2   /* .Config file              */
#define MXFS_EXT_SYSFILE  3   /* kernel / OS internal file */

/* -------------------------------------------------------
 * On-disk directory entry (64 bytes, fixed size)
 * -------------------------------------------------------*/
#define MONOXFS_DIRENT_SZ    64
#define MONOXFS_DIRENT_NAME  55

struct __packed monoxfs_dirent {
    u32  ino;
    u8   type;
    char name[MONOXFS_DIRENT_NAME];
    u8   _pad[4];           /* 4 + 1 + 55 + 4 = 64 */
};
_Static_assert(sizeof(struct monoxfs_dirent) == MONOXFS_DIRENT_SZ, "dirent must be 64 bytes");

/* -------------------------------------------------------
 * In-memory private data per mount
 * -------------------------------------------------------*/
struct monoxfs_priv {
    struct monoxfs_super sb;
    u8                  *inode_bitmap;  /* kmalloc'd */
    u8                  *data_bitmap;
    spinlock_t           bitmap_lock;

    /* Block I/O callbacks (set by block device driver) */
    int (*bread)(struct monoxfs_priv *priv, u64 blk, void *buf);
    int (*bwrite)(struct monoxfs_priv *priv, u64 blk, const void *buf);
    void *blk_dev;   /* opaque handle from block driver */

    bool  read_only; /* mounted with the "ro" option */
    bool  dirty;     /* bitmaps/superblock need to be written back */
};

/* -------------------------------------------------------
 * Bitmap helpers
 * -------------------------------------------------------*/
static int bitmap_alloc(u8 *bm, u64 n)
{
    for (u64 i = 0; i < n; i++) {
        u64 byte = i / 8;
        u8  bit  = 1 << (i % 8);
        if (!(bm[byte] & bit)) {
            bm[byte] |= bit;
            return (int)i;
        }
    }
    return -1;
}

static void bitmap_free(u8 *bm, u64 idx)
{
    bm[idx / 8] &= (u8)~(1u << (idx % 8));
}

/* -------------------------------------------------------
 * Read/write inode from disk
 * -------------------------------------------------------*/
static int monoxfs_read_inode(struct monoxfs_priv *p, u64 ino,
                               struct monoxfs_inode *out)
{
    if (ino >= p->sb.nr_inodes) return MX_EINVAL;
    u64 inodes_per_block = p->sb.block_size / p->sb.inode_size;
    u64 block = p->sb.inode_table_start + ino / inodes_per_block;
    u64 off   = (ino % inodes_per_block) * p->sb.inode_size;
    if (block >= p->sb.nr_blocks || off + sizeof(*out) > MONOXFS_BLOCK_SZ)
        return MX_EIO;

    u8 buf[MONOXFS_BLOCK_SZ];
    int ret = p->bread(p, block, buf);
    if (ret) return ret;

    memcpy(out, buf + off, sizeof(*out));
    return MX_OK;
}

static int monoxfs_write_inode(struct monoxfs_priv *p, u64 ino,
                                const struct monoxfs_inode *in)
{
    if (p->read_only) return MX_EACCES;
    if (ino >= p->sb.nr_inodes) return MX_EINVAL;
    u64 inodes_per_block = p->sb.block_size / p->sb.inode_size;
    u64 block = p->sb.inode_table_start + ino / inodes_per_block;
    u64 off   = (ino % inodes_per_block) * p->sb.inode_size;
    if (block >= p->sb.nr_blocks || off + sizeof(*in) > MONOXFS_BLOCK_SZ)
        return MX_EIO;

    u8 buf[MONOXFS_BLOCK_SZ];
    int ret = p->bread(p, block, buf);
    if (ret) return ret;

    memcpy(buf + off, in, sizeof(*in));
    return p->bwrite(p, block, buf);
}

/* -------------------------------------------------------
 * Allocate / free a data block
 * -------------------------------------------------------*/
static s64 monoxfs_alloc_block(struct monoxfs_priv *p)
{
    spin_lock(&p->bitmap_lock);
    int idx = bitmap_alloc(p->data_bitmap, p->sb.nr_blocks - p->sb.data_start);
    if (idx < 0) {
        spin_unlock(&p->bitmap_lock);
        return MX_ENOSPC;
    }
    p->sb.nr_free_blocks--;
    p->dirty = true;
    spin_unlock(&p->bitmap_lock);
    return (s64)(p->sb.data_start + (u64)idx);
}

static void __attribute__((unused)) monoxfs_free_block(struct monoxfs_priv *p, u64 blk)
{
    if (blk < p->sb.data_start || blk >= p->sb.nr_blocks) return;
    spin_lock(&p->bitmap_lock);
    bitmap_free(p->data_bitmap, blk - p->sb.data_start);
    p->sb.nr_free_blocks++;
    p->dirty = true;
    spin_unlock(&p->bitmap_lock);
}

/* -------------------------------------------------------
 * Allocate / free an inode
 * -------------------------------------------------------*/
static s64 __attribute__((unused)) monoxfs_alloc_inode(struct monoxfs_priv *p)
{
    spin_lock(&p->bitmap_lock);
    int idx = bitmap_alloc(p->inode_bitmap, p->sb.nr_inodes);
    if (idx < 0) {
        spin_unlock(&p->bitmap_lock);
        return MX_ENOSPC;
    }
    p->sb.nr_free_inodes--;
    p->dirty = true;
    spin_unlock(&p->bitmap_lock);
    return (s64)idx;
}

/* -------------------------------------------------------
 * Map file byte offset to block number (with indirect)
 *   returns MX_ENOENT for a hole when !alloc
 *   *fresh is set when the block was newly allocated by this call
 * -------------------------------------------------------*/
static s64 monoxfs_get_block(struct monoxfs_priv *p,
                               const struct monoxfs_inode *ino,
                               u64 offset, bool alloc,
                               struct monoxfs_inode *ino_mut,
                               bool *fresh)
{
    u64 blk_off = offset / p->sb.block_size;
    u32 ptr_per_blk = (u32)(p->sb.block_size / sizeof(u32));
    if (fresh) *fresh = false;

    /* Direct */
    if (blk_off < MONOXFS_N_DIRECT) {
        if (ino->direct[blk_off]) return (s64)ino->direct[blk_off];
        if (!alloc) return MX_ENOENT;
        s64 b = monoxfs_alloc_block(p);
        if (b < 0) return b;
        ino_mut->direct[blk_off] = (u32)b;
        if (fresh) *fresh = true;
        return b;
    }

    blk_off -= MONOXFS_N_DIRECT;

    /* Single indirect */
    if (blk_off < ptr_per_blk) {
        u32 ind_blk[MONOXFS_BLOCK_SZ / sizeof(u32)];
        u32 ind_no;

        if (!ino->indirect) {
            if (!alloc) return MX_ENOENT;
            s64 b = monoxfs_alloc_block(p);
            if (b < 0) return b;
            memset(ind_blk, 0, sizeof(ind_blk));
            /* persist the empty pointer block BEFORE the inode refers to it
             * (it used to contain whatever the disk held before) */
            if (p->bwrite(p, (u64)b, ind_blk)) {
                monoxfs_free_block(p, (u64)b);
                return MX_EIO;
            }
            ino_mut->indirect = (u32)b;
            ind_no = (u32)b;
        } else {
            ind_no = ino->indirect;
            if (p->bread(p, ind_no, ind_blk))     /* was unchecked: garbage pointers */
                return MX_EIO;
        }

        if (ind_blk[blk_off]) return (s64)ind_blk[blk_off];
        if (!alloc) return MX_ENOENT;
        s64 b = monoxfs_alloc_block(p);
        if (b < 0) return b;
        ind_blk[blk_off] = (u32)b;
        if (p->bwrite(p, ind_no, ind_blk)) {
            monoxfs_free_block(p, (u64)b);
            return MX_EIO;
        }
        if (fresh) *fresh = true;
        return b;
    }

    /* Double indirect (for files > ~4 MiB) */
    /* Simplified: return ERANGE for now (single indirect covers 1 GiB w/ 4K blocks) */
    pr_err("MonoxFS: double indirect block access not supported yet\n");
    return MX_ERANGE;
}

/* -------------------------------------------------------
 * VFS file operations
 * -------------------------------------------------------*/
static s64 monoxfs_read(struct vfs_file *f, u8 *buf, size_t len)
{
    struct vfs_inode   *vi  = f->inode;
    struct monoxfs_priv *p  = (vi && vi->mount) ? vi->mount->fs_priv : NULL;
    if (!p) return MX_EIO;

    if (f->pos < 0) return MX_EINVAL;
    u64 file_off = (u64)f->pos;
    u64 file_sz  = vi->size;
    if (file_off >= file_sz) return 0;
    if (len > file_sz - file_off) len = (size_t)(file_sz - file_off);   /* no overflow */

    struct monoxfs_inode disk_ino;
    if (monoxfs_read_inode(p, vi->ino, &disk_ino)) return MX_EIO;

    size_t done = 0;
    while (done < len) {
        u64 blk_off   = (file_off + done) / p->sb.block_size;
        u64 blk_inner = (file_off + done) % p->sb.block_size;
        u64 can_read  = MIN(p->sb.block_size - blk_inner, len - done);

        s64 blk = monoxfs_get_block(p, &disk_ino, blk_off * p->sb.block_size,
                                    false, NULL, NULL);
        if (blk == MX_ENOENT) {
            /* sparse hole inside the file: reads as zeros */
            memset(buf + done, 0, (size_t)can_read);
            done += (size_t)can_read;
            continue;
        }
        if (blk < 0) {
            if (!done) return blk;
            break;
        }

        u8 tmp[MONOXFS_BLOCK_SZ];
        if (p->bread(p, (u64)blk, tmp)) {
            if (!done) return MX_EIO;
            break;
        }
        memcpy(buf + done, tmp + blk_inner, can_read);
        done += (size_t)can_read;
    }

    f->pos += (off_t)done;
    return (s64)done;
}

static s64 monoxfs_write(struct vfs_file *f, const u8 *buf, size_t len)
{
    struct vfs_inode   *vi = f->inode;
    struct monoxfs_priv *p = (vi && vi->mount) ? vi->mount->fs_priv : NULL;
    if (!p) return MX_EIO;
    if (p->read_only) return MX_EACCES;          /* "ro" mounts are read-only now */
    if (vi->type == INODE_DIR) return MX_EINVAL;

    struct monoxfs_inode disk_ino;
    if (monoxfs_read_inode(p, vi->ino, &disk_ino)) return MX_EIO;

    if (f->flags & VFS_APPEND)
        f->pos = (off_t)disk_ino.size;
    if (f->pos < 0) return MX_EINVAL;

    size_t done = 0;
    u64 file_off = (u64)f->pos;
    s64 err = 0;

    while (done < len) {
        u64 blk_off   = (file_off + done) / p->sb.block_size;
        u64 blk_inner = (file_off + done) % p->sb.block_size;
        u64 can_write = MIN(p->sb.block_size - blk_inner, len - done);

        bool fresh = false;
        s64 blk = monoxfs_get_block(p, &disk_ino, blk_off * p->sb.block_size,
                                    false, NULL, NULL);
        if (blk == MX_ENOENT) {
            blk = monoxfs_get_block(p, &disk_ino, blk_off * p->sb.block_size,
                                    true, &disk_ino, &fresh);
        }
        if (blk < 0) { err = blk; break; }

        u8 tmp[MONOXFS_BLOCK_SZ];
        if (can_write < p->sb.block_size) {
            if (fresh) {
                memset(tmp, 0, sizeof(tmp));     /* don't leak old disk contents */
            } else if (p->bread(p, (u64)blk, tmp)) {
                err = MX_EIO;                    /* was unchecked */
                break;
            }
        }
        memcpy(tmp + blk_inner, buf + done, can_write);
        if (p->bwrite(p, (u64)blk, tmp)) { err = MX_EIO; break; }
        done += (size_t)can_write;
    }

    f->pos += (off_t)done;
    if ((u64)f->pos > disk_ino.size)
        disk_ino.size = (u64)f->pos;
    vi->size = disk_ino.size;
    /* always persist: blocks may have been allocated even if the write
     * failed half way, and the inode must reference them */
    if (monoxfs_write_inode(p, vi->ino, &disk_ino) && !done)
        return MX_EIO;

    if (!done && err) return err;
    return (s64)done;
}

/* -------------------------------------------------------
 * Flush bitmaps + superblock (the old driver never wrote them back)
 * -------------------------------------------------------*/
static int monoxfs_flush(struct monoxfs_priv *p)
{
    if (p->read_only || !p->dirty) return MX_OK;

    u8 blk[MONOXFS_BLOCK_SZ];
    int ret = MX_OK;

    u64 ino_bm_bytes = ALIGN(p->sb.nr_inodes, 8) / 8;
    u64 dat_bm_bytes = ALIGN(p->sb.nr_blocks, 8) / 8;

    /* snapshot "dirty" first: allocations racing with us re-set it */
    spin_lock(&p->bitmap_lock);
    p->dirty = false;
    spin_unlock(&p->bitmap_lock);

    u64 done = 0;
    for (u64 b = p->sb.inode_bitmap_start; b < p->sb.data_bitmap_start && done < ino_bm_bytes; b++) {
        size_t chunk = (size_t)MIN((u64)MONOXFS_BLOCK_SZ, ino_bm_bytes - done);
        memset(blk, 0, sizeof(blk));
        memcpy(blk, p->inode_bitmap + done, chunk);
        if (p->bwrite(p, b, blk)) ret = MX_EIO;
        done += chunk;
    }

    done = 0;
    for (u64 b = p->sb.data_bitmap_start; b < p->sb.inode_table_start && done < dat_bm_bytes; b++) {
        size_t chunk = (size_t)MIN((u64)MONOXFS_BLOCK_SZ, dat_bm_bytes - done);
        memset(blk, 0, sizeof(blk));
        memcpy(blk, p->data_bitmap + done, chunk);
        if (p->bwrite(p, b, blk)) ret = MX_EIO;
        done += chunk;
    }

    /* superblock: read-modify-write so the rest of block 0 survives */
    if (p->bread(p, 0, blk) == 0) {
        memcpy(blk, &p->sb, sizeof(p->sb));
        if (p->bwrite(p, 0, blk)) ret = MX_EIO;
    } else {
        ret = MX_EIO;
    }

    if (ret) {
        spin_lock(&p->bitmap_lock);
        p->dirty = true;                 /* try again next time */
        spin_unlock(&p->bitmap_lock);
    }
    return ret;
}

static int monoxfs_sync(struct vfs_file *f)
{
    if (!f || !f->inode || !f->inode->mount) return MX_OK;
    struct monoxfs_priv *p = f->inode->mount->fs_priv;
    return p ? monoxfs_flush(p) : MX_OK;
}

static s64 monoxfs_seek(struct vfs_file *f, off_t off, int whence)
{
    off_t new_pos;
    switch (whence) {
    case VFS_SEEK_SET: new_pos = off; break;
    case VFS_SEEK_CUR: new_pos = f->pos + off; break;
    case VFS_SEEK_END: new_pos = (off_t)f->inode->size + off; break;
    default: return MX_EINVAL;
    }
    if (new_pos < 0) return MX_EINVAL;
    f->pos = new_pos;
    return new_pos;
}

static int monoxfs_stat(struct vfs_inode *inode, struct vfs_stat *st)
{
    st->ino   = inode->ino;
    st->type  = inode->type;
    st->mode  = inode->mode;
    st->uid   = inode->uid;
    st->gid   = inode->gid;
    st->size  = inode->size;
    st->mtime = inode->mtime;
    st->atime = inode->atime;
    st->ctime = inode->ctime;
    st->nlink = inode->nlink;
    return MX_OK;
}

/* -------------------------------------------------------
 * VFS directory operations
 * -------------------------------------------------------*/
static int monoxfs_readdir(struct vfs_file *f, struct vfs_dirent *de)
{
    struct vfs_inode   *vi = f->inode;
    struct monoxfs_priv *p = (vi && vi->mount) ? vi->mount->fs_priv : NULL;
    if (!p) return MX_EIO;
    if (vi->type != INODE_DIR) return MX_ENOTDIR;

    while ((u64)f->pos < vi->size) {
        struct monoxfs_dirent disk_de;
        struct vfs_file tmp_f = *f;
        s64 n = monoxfs_read(&tmp_f, (u8 *)&disk_de, sizeof(disk_de));
        f->pos = tmp_f.pos;
        if (n < (s64)sizeof(disk_de)) return MX_ENOENT;
        if (!disk_de.ino) continue; /* deleted entry */

        de->ino = disk_de.ino;
        strncpy(de->name, disk_de.name, MONOXFS_DIRENT_NAME);
        de->name[MONOXFS_DIRENT_NAME] = '\0';
        de->type = (inode_type_t)disk_de.type;
        return MX_OK;
    }
    return MX_ENOENT;
}

static int monoxfs_lookup(struct vfs_dentry *parent, const char *name,
                           struct vfs_dentry **out)
{
    struct vfs_inode   *vi = parent->inode;
    struct monoxfs_priv *p = (vi && vi->mount) ? vi->mount->fs_priv : NULL;
    if (!p || vi->type != INODE_DIR) return MX_ENOTDIR;

    struct vfs_file f = {
        .inode = vi,
        .ops   = vi->ops,
        .pos   = 0,
    };

    struct vfs_dirent de;
    while (monoxfs_readdir(&f, &de) == MX_OK) {
        if (strcmp(de.name, name) == 0) {
            struct monoxfs_inode disk_ino;
            if (monoxfs_read_inode(p, de.ino, &disk_ino)) return MX_EIO;

            struct vfs_inode *new_vi = kzalloc(sizeof(*new_vi));
            if (!new_vi) return MX_ENOMEM;
            new_vi->ino   = de.ino;
            new_vi->type  = de.type;
            new_vi->mode  = disk_ino.mode;
            new_vi->uid   = disk_ino.uid;
            new_vi->gid   = disk_ino.gid;
            new_vi->size  = disk_ino.size;
            new_vi->mtime = disk_ino.mtime;
            new_vi->atime = disk_ino.atime;
            new_vi->ctime = disk_ino.ctime;
            new_vi->nlink = disk_ino.nlink;
            new_vi->ops   = vi->ops;
            new_vi->mount = vi->mount;
            atomic_set(&new_vi->refcount, 1);

            struct vfs_dentry *d = kzalloc(sizeof(*d));
            if (!d) { kfree(new_vi); return MX_ENOMEM; }
            strncpy(d->name, name, 255);
            list_init(&d->children);
            list_init(&d->child_node);
            spin_lock_init(&d->lock);
            atomic_set(&d->refcount, 1);
            d->inode = new_vi;
            *out = d;
            return MX_OK;
        }
    }
    return MX_ENOENT;
}

/* -------------------------------------------------------
 * Mount
 * -------------------------------------------------------*/
static void monoxfs_free_priv(struct monoxfs_priv *p)
{
    if (!p) return;
    if (p->inode_bitmap) kfree(p->inode_bitmap);
    if (p->data_bitmap)  kfree(p->data_bitmap);
    kfree(p);
}

/* "ro" / "rw" (comma separated) */
static bool opts_has(const char *opts, const char *word)
{
    if (!opts) return false;
    size_t wl = strlen(word);
    const char *s = opts;
    while (*s) {
        const char *e = s;
        while (*e && *e != ',') e++;
        if ((size_t)(e - s) == wl && strncmp(s, word, wl) == 0) return true;
        s = *e ? e + 1 : e;
    }
    return false;
}

static int monoxfs_unmount(struct vfs_mount *mnt);

static int monoxfs_mount(struct vfs_mount *mnt, const char *dev,
                          const char *opts)
{
    struct monoxfs_priv *p = kzalloc(sizeof(*p));
    if (!p) return MX_ENOMEM;

    /* Register block device I/O (resolved from 'dev' string by block layer) */
    extern int blk_find_dev(const char *name,
                            int (**bread)(struct monoxfs_priv *, u64, void *),
                            int (**bwrite)(struct monoxfs_priv *, u64, const void *),
                            void **handle);
    spin_lock_init(&p->bitmap_lock);
    p->read_only = opts_has(opts, "ro");

    int ret = blk_find_dev(dev, &p->bread, &p->bwrite, &p->blk_dev);
    if (ret != MX_OK) {
        pr_warn("MonoxFS: block device '%s' not found: %d\n", dev ? dev : "(null)", ret);
        kfree(p);
        return ret;
    }
    /* a "successful" lookup that returned no I/O callbacks would be a NULL call */
    if (!p->bread || (!p->bwrite && !p->read_only)) {
        pr_err("MonoxFS: block device '%s' has no I/O callbacks\n", dev ? dev : "(null)");
        kfree(p);
        return MX_ENODEV;
    }

    /* Read superblock: a whole block goes into a TEMPORARY buffer; reading
     * it straight into &p->sb overflowed the structure by ~3.7 KiB. */
    u8 blk_buf[MONOXFS_BLOCK_SZ];
    if (p->bread(p, 0, blk_buf)) { kfree(p); return MX_EIO; }
    memcpy(&p->sb, blk_buf, sizeof(p->sb));

    if (p->sb.magic != MONOXFS_MAGIC) {
        pr_err("MonoxFS: bad magic on '%s' (got 0x%08x)\n", dev, p->sb.magic);
        kfree(p);
        return MX_EIO;
    }
    if (p->sb.version != MONOXFS_VERSION) {
        pr_err("MonoxFS: version mismatch (got %u)\n", p->sb.version);
        kfree(p);
        return MX_EIO;
    }

    /* Validate the geometry before trusting any of it */
    struct monoxfs_super *sb = &p->sb;
    u64 inodes_per_block = (sb->inode_size && sb->inode_size <= MONOXFS_BLOCK_SZ)
                         ? MONOXFS_BLOCK_SZ / sb->inode_size : 0;
    u64 ino_bm_bytes = ALIGN(sb->nr_inodes, 8) / 8;
    u64 dat_bm_bytes = ALIGN(sb->nr_blocks, 8) / 8;
    bool sane =
        sb->block_size == MONOXFS_BLOCK_SZ &&
        sb->inode_size >= sizeof(struct monoxfs_inode) &&
        sb->inode_size <= MONOXFS_BLOCK_SZ &&
        (MONOXFS_BLOCK_SZ % sb->inode_size) == 0 &&
        sb->nr_blocks > 0 && sb->nr_blocks < (1ULL << 40) &&
        sb->nr_inodes > 2 && sb->nr_inodes < (1ULL << 36) &&
        sb->inode_bitmap_start <= sb->data_bitmap_start &&
        sb->data_bitmap_start  <= sb->inode_table_start &&
        sb->inode_table_start  <= sb->data_start &&
        sb->data_start         <  sb->nr_blocks &&
        ino_bm_bytes <= (sb->data_bitmap_start - sb->inode_bitmap_start) * MONOXFS_BLOCK_SZ &&
        dat_bm_bytes <= (sb->inode_table_start - sb->data_bitmap_start) * MONOXFS_BLOCK_SZ &&
        inodes_per_block &&
        sb->nr_inodes <= (sb->data_start - sb->inode_table_start) * inodes_per_block;
    if (!sane) {
        pr_err("MonoxFS: '%s' has an invalid superblock geometry\n", dev);
        kfree(p);
        return MX_EIO;
    }

    /* Load bitmaps */
    p->inode_bitmap  = kzalloc(ino_bm_bytes);
    p->data_bitmap   = kzalloc(dat_bm_bytes);
    if (!p->inode_bitmap || !p->data_bitmap) { monoxfs_free_priv(p); return MX_ENOMEM; }

    u64 got = 0;
    for (u64 b = sb->inode_bitmap_start; b < sb->data_bitmap_start && got < ino_bm_bytes; b++) {
        if (p->bread(p, b, blk_buf)) { monoxfs_free_priv(p); return MX_EIO; }
        size_t chunk = (size_t)MIN((u64)MONOXFS_BLOCK_SZ, ino_bm_bytes - got);
        memcpy(p->inode_bitmap + got, blk_buf, chunk);
        got += chunk;
    }

    got = 0;
    for (u64 b = sb->data_bitmap_start; b < sb->inode_table_start && got < dat_bm_bytes; b++) {
        if (p->bread(p, b, blk_buf)) { monoxfs_free_priv(p); return MX_EIO; }
        size_t chunk = (size_t)MIN((u64)MONOXFS_BLOCK_SZ, dat_bm_bytes - got);
        memcpy(p->data_bitmap + got, blk_buf, chunk);
        got += chunk;
    }

    mnt->fs_priv = p;

    /* Create root inode (inode 2, UNIX convention) */
    struct monoxfs_inode root_disk;
    ret = monoxfs_read_inode(p, 2, &root_disk);
    if (ret) {
        mnt->fs_priv = NULL;
        monoxfs_free_priv(p);
        return MX_EIO;
    }

    struct vfs_inode *root_vi = kzalloc(sizeof(*root_vi));
    if (!root_vi) { mnt->fs_priv = NULL; monoxfs_free_priv(p); return MX_ENOMEM; }
    root_vi->ino   = 2;
    root_vi->type  = INODE_DIR;
    root_vi->mode  = root_disk.mode;
    root_vi->size  = root_disk.size;
    root_vi->ops   = &monoxfs_ops;
    root_vi->mount = mnt;
    atomic_set(&root_vi->refcount, 1);

    struct vfs_dentry *root_d = kzalloc(sizeof(*root_d));
    if (!root_d) { kfree(root_vi); mnt->fs_priv = NULL; monoxfs_free_priv(p); return MX_ENOMEM; }
    strncpy(root_d->name, "/", 1);
    list_init(&root_d->children);
    list_init(&root_d->child_node);
    spin_lock_init(&root_d->lock);
    atomic_set(&root_d->refcount, 1);
    root_d->inode = root_vi;
    root_d->parent= root_d;

    mnt->root = root_d;

    pr_info("MonoxFS: mounted '%s' %s label='%.63s' partition_role=%u\n",
            dev, p->read_only ? "(ro)" : "(rw)", p->sb.label, p->sb.partition_role);
    pr_info("MonoxFS: %llu blocks (%llu free), %llu inodes (%llu free)\n",
            p->sb.nr_blocks, p->sb.nr_free_blocks,
            p->sb.nr_inodes,  p->sb.nr_free_inodes);
    return MX_OK;
}

/* Called by vfs_unmount() once nothing references the mount any more.
 * The root dentry/inode are released by the VFS. */
static int monoxfs_unmount(struct vfs_mount *mnt)
{
    struct monoxfs_priv *p = mnt->fs_priv;
    if (!p) return MX_OK;

    int ret = monoxfs_flush(p);
    if (ret) pr_err("MonoxFS: flush on unmount failed (%d)\n", ret);

    mnt->fs_priv = NULL;
    monoxfs_free_priv(p);
    return ret;
}

/* -------------------------------------------------------
 * fs_ops table (one table: inodes and the registry share it. The old code
 * had two identical copies, one private to the inodes)
 * -------------------------------------------------------*/
struct fs_ops monoxfs_ops = {
    .lookup   = monoxfs_lookup,
    .stat     = monoxfs_stat,
    .read     = monoxfs_read,
    .write    = monoxfs_write,
    .seek     = monoxfs_seek,
    .sync     = monoxfs_sync,
    .readdir  = monoxfs_readdir,
    .mount    = monoxfs_mount,
    .unmount  = monoxfs_unmount,
};
