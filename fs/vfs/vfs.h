/* MONOX Kernel - Virtual Filesystem Switch Interface */

#ifndef _MONOX_VFS_H
#define _MONOX_VFS_H

#include <kernel/types.h>
#include <kernel/kernel.h>   /* spinlock_t, atomic_t, list_head used below */

/* -------------------------------------------------------
 * File open flags
 * -------------------------------------------------------*/
#define VFS_RDONLY   0x00
#define VFS_WRONLY   0x01
#define VFS_RDWR     0x02
#define VFS_CREAT    0x04
#define VFS_APPEND   0x08
#define VFS_TRUNC    0x10
#define VFS_EXCL     0x20

/* -------------------------------------------------------
 * Inode types
 * -------------------------------------------------------*/
typedef enum {
    INODE_REG    = 0,   /* regular file     */
    INODE_DIR    = 1,   /* directory        */
    INODE_LINK   = 2,   /* symbolic link    */
    INODE_CHARDEV= 3,   /* character device */
    INODE_BLKDEV = 4,   /* block device     */
    INODE_PIPE   = 5,   /* pipe / FIFO      */
    INODE_SOCK   = 6,   /* socket           */
} inode_type_t;

/* -------------------------------------------------------
 * Filesystem operations vtable
 * -------------------------------------------------------*/
struct vfs_inode;
struct vfs_file;
struct vfs_dentry;
struct vfs_mount;
struct vfs_dirent;
struct vfs_stat;      /* was first mentioned inside a prototype => wrong scope */

struct fs_ops {
    /* Inode ops */
    int  (*lookup)   (struct vfs_dentry *parent, const char *name,
                       struct vfs_dentry **out);
    int  (*create)   (struct vfs_dentry *parent, const char *name,
                       mode_t mode, struct vfs_dentry **out);
    int  (*mkdir)    (struct vfs_dentry *parent, const char *name, mode_t mode);
    int  (*unlink)   (struct vfs_dentry *parent, const char *name);
    int  (*rmdir)    (struct vfs_dentry *parent, const char *name);
    int  (*rename)   (struct vfs_dentry *old_parent, const char *old_name,
                       struct vfs_dentry *new_parent, const char *new_name);
    int  (*stat)     (struct vfs_inode *inode, struct vfs_stat *stat);

    /* File ops */
    s64  (*read)     (struct vfs_file *f, u8 *buf, size_t len);
    s64  (*write)    (struct vfs_file *f, const u8 *buf, size_t len);
    s64  (*seek)     (struct vfs_file *f, off_t off, int whence);
    int  (*sync)     (struct vfs_file *f);
    int  (*truncate) (struct vfs_file *f, off_t len);
    int  (*readdir)  (struct vfs_file *f, struct vfs_dirent *de);

    /* Lifecycle */
    int  (*mount)    (struct vfs_mount *mnt, const char *dev,
                       const char *opts);
    int  (*unmount)  (struct vfs_mount *mnt);
    void (*inode_put)(struct vfs_inode *inode);
};

/* -------------------------------------------------------
 * Inode
 * -------------------------------------------------------*/
struct vfs_inode {
    ino_t           ino;
    inode_type_t    type;
    mode_t          mode;
    uid_t           uid;
    gid_t           gid;
    u64             size;
    u64             ctime;
    u64             mtime;
    u64             atime;
    nlink_t         nlink;
    dev_t           dev;
    atomic_t        refcount;
    struct fs_ops  *ops;
    struct vfs_mount *mount;
    void           *fs_data;  /* filesystem-private data */
};

/* -------------------------------------------------------
 * Directory entry (dcache)
 * -------------------------------------------------------*/
struct vfs_dentry {
    char             name[256];
    struct vfs_inode *inode;
    struct vfs_dentry *parent;
    struct list_head  children;
    struct list_head  child_node;
    spinlock_t        lock;
    atomic_t          refcount;
};

/* -------------------------------------------------------
 * Open file descriptor
 * -------------------------------------------------------*/
struct vfs_file {
    struct vfs_inode *inode;
    struct vfs_dentry *dentry;
    u32               flags;
    off_t             pos;
    atomic_t          refcount;
    struct fs_ops    *ops;
};

/* -------------------------------------------------------
 * Mount point
 * -------------------------------------------------------*/
struct vfs_mount {
    char              dev[128];
    char              mnt_path[256];
    char              fs_type[32];
    struct vfs_dentry *root;
    struct vfs_dentry *mnt_point;
    struct fs_ops    *ops;
    struct list_head  node;
    void             *fs_priv;
    struct vfs_inode *covered_inode;  /* inode hidden by this mount (restored on unmount) */
};

/* -------------------------------------------------------
 * Stat
 * -------------------------------------------------------*/
struct vfs_stat {
    ino_t       ino;
    inode_type_t type;
    mode_t      mode;
    uid_t       uid;
    gid_t       gid;
    u64         size;
    u64         mtime;
    u64         atime;
    u64         ctime;
    nlink_t     nlink;
    dev_t       dev;
};

/* -------------------------------------------------------
 * Directory entry read result
 * -------------------------------------------------------*/
struct vfs_dirent {
    ino_t         ino;
    inode_type_t  type;
    char          name[256];
};

/* Opaque dir handle */
struct vfs_dir;

/* -------------------------------------------------------
 * VFS public API
 * -------------------------------------------------------*/
int          vfs_init(void);
int          vfs_mount_root(void);
int          vfs_register_fs(const char *name, struct fs_ops *ops);
int          vfs_mount(const char *dev, const char *path,
                        const char *fs_type, const char *opts);
int          vfs_unmount(const char *path);

struct vfs_file *vfs_open(const char *path, u32 flags);
void         vfs_close(struct vfs_file *f);
s64          vfs_read(struct vfs_file *f, void *buf, size_t len);
s64          vfs_write(struct vfs_file *f, const void *buf, size_t len);
s64          vfs_seek(struct vfs_file *f, off_t off, int whence);
int          vfs_stat(const char *path, struct vfs_stat *st);
int          vfs_mkdir(const char *path, mode_t mode);
int          vfs_unlink(const char *path);
int          vfs_rmdir(const char *path);
int          vfs_rename(const char *old_path, const char *new_path);
int          vfs_sync_all(void);

struct vfs_dir  *vfs_opendir(const char *path);
int          vfs_readdir(struct vfs_dir *dir, struct vfs_dirent *de);
void         vfs_closedir(struct vfs_dir *dir);

/* Inode ref counting */
void         vfs_inode_get(struct vfs_inode *inode);
void         vfs_inode_put(struct vfs_inode *inode);

/* Seek whence */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

#endif /* _MONOX_VFS_H */
