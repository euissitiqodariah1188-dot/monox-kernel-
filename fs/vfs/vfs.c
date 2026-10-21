/* MONOX Kernel - Virtual Filesystem Switch (VFS) Implementation
 *
 * Storage layout (requirement):
 *   Partition 0: SYSTEM  - kernel, OS files
 *   Partition 1: USER    - user data, apps, configs
 *
 * File types supported:
 *   .Runap   - runnable application bundle
 *   .Config  - configuration file
 *   (plus all standard types: dir, link, etc.)
 */

#include <kernel/types.h>
#include <kernel/kernel.h>
#include "vfs.h"
#include "../../mm/vmm.h"

/* -------------------------------------------------------
 * Mount table
 * -------------------------------------------------------*/
#define MAX_MOUNTS   32
#define MAX_FS_TYPES 16

static LIST_HEAD(g_mount_list);
static spinlock_t g_mount_lock = SPIN_LOCK_INIT;

struct fs_type_entry {
    char           name[32];
    struct fs_ops *ops;
    bool           valid;
};
static struct fs_type_entry g_fs_types[MAX_FS_TYPES];
static spinlock_t           g_fs_type_lock = SPIN_LOCK_INIT;

/* Root dentry of the whole VFS tree */
static struct vfs_dentry *g_root = NULL;

/* -------------------------------------------------------
 * dentry helpers
 * -------------------------------------------------------*/
static struct vfs_dentry *dentry_alloc(const char *name)
{
    struct vfs_dentry *d = kzalloc(sizeof(*d));
    if (!d) return NULL;
    if (name) strncpy(d->name, name, 255);
    list_init(&d->children);
    list_init(&d->child_node);
    spin_lock_init(&d->lock);
    atomic_set(&d->refcount, 1);
    return d;
}

static void dentry_get(struct vfs_dentry *d)
{
    atomic_inc_return(&d->refcount);
}

static void dentry_put(struct vfs_dentry *d)
{
    if (!d) return;
    if (atomic_dec_and_test(&d->refcount)) {
        /* Never leave a dangling pointer in the parent's child list */
        struct vfs_dentry *p = d->parent;
        if (p && p != d && d->child_node.next && d->child_node.next != &d->child_node) {
            spin_lock(&p->lock);
            list_del_init(&d->child_node);
            spin_unlock(&p->lock);
        }
        if (d->inode)
            vfs_inode_put(d->inode);
        kfree(d);
    }
}

/* Split "/a/b/c" into parent "/a/b" and base "c". The result is one
 * kmalloc'd buffer (free it with kfree(*buf)); *parent and *base point
 * into it. Fixes the old open-coded splitting, which wrote before the
 * buffer for an empty path and mangled relative paths. */
static int split_path(const char *path, char **buf, const char **parent,
                      const char **base)
{
    if (!path || path[0] != '/') return MX_EINVAL;

    size_t plen = strlen(path);
    while (plen > 1 && path[plen - 1] == '/') plen--;   /* trailing slashes */
    if (plen < 2) return MX_EINVAL;                      /* "/" has no parent */

    char *b = kmalloc(plen + 3);
    if (!b) return MX_ENOMEM;
    memcpy(b, path, plen);
    b[plen] = '\0';

    char *s = b + plen - 1;
    while (s > b && *s != '/') s--;                      /* s points at the last '/' */

    if (strlen(s + 1) > 255) { kfree(b); return MX_EINVAL; }

    if (s == b) {
        /* parent is the root: move base up to make room for "/\0" */
        memmove(b + 2, s + 1, strlen(s + 1) + 1);
        b[0] = '/'; b[1] = '\0';
        *base = b + 2;
    } else {
        *s = '\0';
        *base = s + 1;
    }
    *parent = b;
    *buf = b;
    return MX_OK;
}

/* -------------------------------------------------------
 * vfs_init - initialise VFS
 * -------------------------------------------------------*/
int vfs_init(void)
{
    /* Create root dentry */
    g_root = dentry_alloc("/");
    if (!g_root) return MX_ENOMEM;
    g_root->parent = g_root;

    pr_info("VFS: initialised (root dentry ready)\n");
    return MX_OK;
}

/* -------------------------------------------------------
 * vfs_register_fs - register a filesystem driver
 * -------------------------------------------------------*/
int vfs_register_fs(const char *name, struct fs_ops *ops)
{
    if (!name || !ops) return MX_EINVAL;

    spin_lock(&g_fs_type_lock);
    /* already registered? (vfs_mount_root() registers on every call; the
     * old code stored a duplicate each time until the table was full) */
    for (int i = 0; i < MAX_FS_TYPES; i++) {
        if (g_fs_types[i].valid && strcmp(g_fs_types[i].name, name) == 0) {
            spin_unlock(&g_fs_type_lock);
            return MX_EEXIST;
        }
    }
    for (int i = 0; i < MAX_FS_TYPES; i++) {
        if (!g_fs_types[i].valid) {
            strncpy(g_fs_types[i].name, name, 31);
            g_fs_types[i].ops   = ops;
            g_fs_types[i].valid = true;
            spin_unlock(&g_fs_type_lock);
            pr_info("VFS: registered filesystem '%s'\n", name);
            return MX_OK;
        }
    }
    spin_unlock(&g_fs_type_lock);
    return MX_ENOSPC;
}

static struct fs_ops *fs_find(const char *name)
{
    spin_lock(&g_fs_type_lock);
    for (int i = 0; i < MAX_FS_TYPES; i++) {
        if (g_fs_types[i].valid && strcmp(g_fs_types[i].name, name) == 0) {
            struct fs_ops *ops = g_fs_types[i].ops;
            spin_unlock(&g_fs_type_lock);
            return ops;
        }
    }
    spin_unlock(&g_fs_type_lock);
    return NULL;
}

/* -------------------------------------------------------
 * path_resolve - resolve a VFS path to a dentry
 *   Handles absolute paths only for now.
 * -------------------------------------------------------*/
static struct vfs_dentry *path_resolve(const char *path)
{
    if (!path || path[0] != '/') return NULL;
    if (!g_root) return NULL;

    struct vfs_dentry *cur = g_root;
    dentry_get(cur);

    if (path[1] == '\0') return cur;  /* "/" */

    /* Walk components */
    char component[256];
    const char *p = path + 1;

    while (*p) {
        /* Extract next component */
        size_t len = 0;
        while (p[len] && p[len] != '/') len++;
        if (!len) { p++; continue; }
        if (len > 255) { dentry_put(cur); return NULL; }

        memcpy(component, p, len);
        component[len] = '\0';
        p += len;
        if (*p == '/') p++;

        /* Handle . and .. */
        if (strcmp(component, ".") == 0) continue;
        if (strcmp(component, "..") == 0) {
            struct vfs_dentry *parent = cur->parent;
            if (parent && parent != cur) {
                dentry_get(parent);
                dentry_put(cur);
                cur = parent;
            }
            continue;
        }

        /* Look in children first (dcache) */
        struct vfs_dentry *child = NULL;
        spin_lock(&cur->lock);
        struct list_head *pos;
        list_for_each(pos, &cur->children) {
            struct vfs_dentry *c = list_entry(pos, struct vfs_dentry, child_node);
            if (strcmp(c->name, component) == 0) {
                child = c;
                dentry_get(child);
                break;
            }
        }
        spin_unlock(&cur->lock);

        if (!child) {
            /* Cache miss: ask filesystem */
            if (!cur->inode || !cur->inode->ops || !cur->inode->ops->lookup) {
                dentry_put(cur);
                return NULL;
            }
            int ret = cur->inode->ops->lookup(cur, component, &child);
            if (ret != MX_OK || !child) {
                dentry_put(cur);
                return NULL;
            }

            /* Add to dcache. Another thread may have cached the same name
             * meanwhile: use theirs and drop ours. */
            child->parent = cur;
            spin_lock(&cur->lock);
            struct vfs_dentry *dup = NULL;
            list_for_each(pos, &cur->children) {
                struct vfs_dentry *c = list_entry(pos, struct vfs_dentry, child_node);
                if (strcmp(c->name, component) == 0) { dup = c; break; }
            }
            if (dup) {
                dentry_get(dup);
                spin_unlock(&cur->lock);
                list_init(&child->child_node);   /* never linked */
                dentry_put(child);
                child = dup;
            } else {
                list_add_tail(&child->child_node, &cur->children);
                /* The dcache owns one reference; the caller gets its own.
                 * Without this the caller's dentry_put() freed a dentry that
                 * was still linked in the parent (use-after-free). */
                dentry_get(child);
                spin_unlock(&cur->lock);
            }
        }

        dentry_put(cur);
        cur = child;
    }

    return cur;
}

/* -------------------------------------------------------
 * vfs_mount
 *
 * Bug-fix pass: the old attach step
 *   - overwrote mnt_point->inode without releasing the covered inode (leak),
 *   - shared mnt->root->inode between two dentries without taking a
 *     reference (double free once both were released),
 *   - did list_add_tail() on nodes that were still linked in the old list
 *     (both lists corrupted).
 * -------------------------------------------------------*/
int vfs_mount(const char *dev, const char *path,
              const char *fs_type, const char *opts)
{
    if (!fs_type || !path) return MX_EINVAL;

    struct fs_ops *ops = fs_find(fs_type);
    if (!ops || !ops->mount) {
        pr_err("VFS: unknown filesystem type '%s'\n", fs_type);
        return MX_ENODEV;
    }

    /* one filesystem per mount point */
    spin_lock(&g_mount_lock);
    struct list_head *mp;
    list_for_each(mp, &g_mount_list) {
        struct vfs_mount *m = list_entry(mp, struct vfs_mount, node);
        if (strcmp(m->mnt_path, path) == 0) {
            spin_unlock(&g_mount_lock);
            return MX_EBUSY;
        }
    }
    spin_unlock(&g_mount_lock);

    struct vfs_dentry *mnt_point = NULL;
    if (strcmp(path, "/") == 0) {
        mnt_point = g_root;
        dentry_get(mnt_point);
    } else {
        mnt_point = path_resolve(path);
        if (!mnt_point) return MX_ENOENT;
    }

    struct vfs_mount *mnt = kzalloc(sizeof(*mnt));
    if (!mnt) { dentry_put(mnt_point); return MX_ENOMEM; }

    if (dev)   strncpy(mnt->dev,      dev,      127);
    strncpy(mnt->mnt_path, path,     255);
    strncpy(mnt->fs_type,  fs_type,   31);
    mnt->ops       = ops;
    mnt->mnt_point = mnt_point;
    list_init(&mnt->node);

    int ret = ops->mount(mnt, dev, opts);
    if (ret != MX_OK) {
        kfree(mnt);
        dentry_put(mnt_point);
        return ret;
    }

    /* Attach root of new fs to mount point */
    if (mnt->root) {
        struct vfs_inode *covered = mnt_point->inode;   /* reference moves to mnt */
        vfs_inode_get(mnt->root->inode);
        mnt_point->inode  = mnt->root->inode;
        mnt->covered_inode = covered;

        spin_lock(&mnt_point->lock);
        struct list_head *pos, *n;
        list_for_each_safe(pos, n, &mnt->root->children) {
            struct vfs_dentry *c = list_entry(pos, struct vfs_dentry, child_node);
            list_del(pos);
            c->parent = mnt_point;
            list_add_tail(pos, &mnt_point->children);
        }
        spin_unlock(&mnt_point->lock);
    }

    spin_lock(&g_mount_lock);
    list_add_tail(&mnt->node, &g_mount_list);
    spin_unlock(&g_mount_lock);

    pr_info("VFS: mounted %s at %s (type=%s)\n",
            dev ? dev : "none", path, fs_type);
    return MX_OK;
}

/* A subtree is busy if any cached descendant is referenced by someone other
 * than the dcache itself (refcount > 1) */
static bool dentry_tree_busy(struct vfs_dentry *d)
{
    bool busy = false;
    spin_lock(&d->lock);
    struct list_head *pos;
    list_for_each(pos, &d->children) {
        struct vfs_dentry *c = list_entry(pos, struct vfs_dentry, child_node);
        if (atomic_read(&c->refcount) > 1 || dentry_tree_busy(c)) { busy = true; break; }
    }
    spin_unlock(&d->lock);
    return busy;
}

/* Drop every cached child (and their children) */
static void dentry_prune_children(struct vfs_dentry *d)
{
    for (;;) {
        struct vfs_dentry *c = NULL;
        spin_lock(&d->lock);
        if (!list_empty(&d->children)) {
            c = list_entry(d->children.next, struct vfs_dentry, child_node);
            list_del_init(&c->child_node);
        }
        spin_unlock(&d->lock);
        if (!c) break;
        dentry_prune_children(c);
        dentry_put(c);                       /* the dcache's reference */
    }
}

/* -------------------------------------------------------
 * vfs_unmount - was declared in vfs.h but never defined, and nothing ever
 * released a filesystem's memory (bitmaps, private data, root dentry).
 * -------------------------------------------------------*/
int vfs_unmount(const char *path)
{
    if (!path) return MX_EINVAL;

    spin_lock(&g_mount_lock);
    struct vfs_mount *mnt = NULL;
    struct list_head *pos;
    list_for_each(pos, &g_mount_list) {
        struct vfs_mount *m = list_entry(pos, struct vfs_mount, node);
        if (strcmp(m->mnt_path, path) == 0) { mnt = m; break; }
    }
    if (!mnt) { spin_unlock(&g_mount_lock); return MX_ENOENT; }

    /* refuse while another mount sits below this one */
    size_t plen = strlen(path);
    list_for_each(pos, &g_mount_list) {
        struct vfs_mount *m = list_entry(pos, struct vfs_mount, node);
        if (m != mnt && strncmp(m->mnt_path, path, plen) == 0 &&
            (path[plen - 1] == '/' || m->mnt_path[plen] == '/')) {
            spin_unlock(&g_mount_lock);
            return MX_EBUSY;
        }
    }
    spin_unlock(&g_mount_lock);

    struct vfs_dentry *mp_d = mnt->mnt_point;
    /* steady state = 1 (dcache/root) + 1 (taken by vfs_mount) */
    if (mp_d && (atomic_read(&mp_d->refcount) > 2 || dentry_tree_busy(mp_d)))
        return MX_EBUSY;                      /* files still open */

    spin_lock(&g_mount_lock);
    list_del_init(&mnt->node);
    spin_unlock(&g_mount_lock);

    if (mp_d) {
        dentry_prune_children(mp_d);
        /* give the mount point its original inode back */
        struct vfs_inode *cur = mp_d->inode;
        mp_d->inode = mnt->covered_inode;
        if (mnt->root) vfs_inode_put(cur);   /* the extra ref taken at mount */
        mnt->covered_inode = NULL;
    }

    int ret = MX_OK;
    if (mnt->ops && mnt->ops->unmount)
        ret = mnt->ops->unmount(mnt);        /* fs frees priv data */

    if (mnt->root) dentry_put(mnt->root);
    if (mp_d) dentry_put(mp_d);              /* reference taken by vfs_mount */
    kfree(mnt);

    pr_info("VFS: unmounted %s\n", path);
    return ret;
}

/* -------------------------------------------------------
 * vfs_mount_root - called by PID 1
 * -------------------------------------------------------*/
int vfs_mount_root(void)
{
    /* monoxfs is registered by fs/monoxfs/monoxfs.c at boot */
    extern struct fs_ops monoxfs_ops;
    int ret = vfs_register_fs("monoxfs", &monoxfs_ops);
    if (ret != MX_OK && ret != MX_EEXIST) return ret;

    /* Mount system partition (partition 0) at / */
    ret = vfs_mount("nvme0p0", "/", "monoxfs", "ro");
    if (ret != MX_OK) return ret;

    /* Mount user partition (partition 1) at /user */
    ret = vfs_mkdir("/user", 0755);
    if (ret != MX_OK) pr_warn("VFS: cannot mkdir /user: %d\n", ret);

    ret = vfs_mount("nvme0p1", "/user", "monoxfs", "rw");
    if (ret != MX_OK) pr_warn("VFS: user partition not available: %d\n", ret);

    return MX_OK;
}

/* -------------------------------------------------------
 * vfs_open
 * -------------------------------------------------------*/
struct vfs_file *vfs_open(const char *path, u32 flags)
{
    if (!path) return NULL;

    struct vfs_dentry *d = path_resolve(path);

    if (d && (flags & VFS_CREAT) && (flags & VFS_EXCL)) {
        dentry_put(d);                        /* O_CREAT|O_EXCL on an existing file */
        return NULL;
    }

    if (!d && (flags & VFS_CREAT)) {
        char *buf; const char *parent_path, *basename;
        if (split_path(path, &buf, &parent_path, &basename) != MX_OK)
            return NULL;

        struct vfs_dentry *parent = path_resolve(parent_path);
        if (!parent) { kfree(buf); return NULL; }
        /* (the old code dereferenced inode->ops without checking it) */
        if (!parent->inode || !parent->inode->ops || !parent->inode->ops->create) {
            dentry_put(parent);
            kfree(buf);
            return NULL;
        }
        int ret = parent->inode->ops->create(parent, basename, 0644, &d);
        dentry_put(parent);
        kfree(buf);                           /* basename lives in buf */
        if (ret != MX_OK || !d) return NULL;
    }

    if (!d) return NULL;

    struct vfs_file *f = kzalloc(sizeof(*f));
    if (!f) { dentry_put(d); return NULL; }

    f->inode   = d->inode;
    f->dentry  = d;
    f->flags   = flags;
    f->pos     = 0;
    f->ops     = d->inode ? d->inode->ops : NULL;
    atomic_set(&f->refcount, 1);

    if ((flags & VFS_TRUNC) && f->ops && f->ops->truncate)
        f->ops->truncate(f, 0);

    return f;
}

/* -------------------------------------------------------
 * vfs_close
 * -------------------------------------------------------*/
void vfs_close(struct vfs_file *f)
{
    if (!f) return;
    if (f->ops && f->ops->sync) f->ops->sync(f);
    if (f->dentry) dentry_put(f->dentry);
    kfree(f);
}

/* -------------------------------------------------------
 * vfs_read / vfs_write / vfs_seek
 * -------------------------------------------------------*/
s64 vfs_read(struct vfs_file *f, void *buf, size_t len)
{
    if (!f || !f->ops || !f->ops->read) return MX_ENOSYS;
    if (!buf || !len) return 0;
    return f->ops->read(f, buf, len);
}

s64 vfs_write(struct vfs_file *f, const void *buf, size_t len)
{
    if (!f || !f->ops || !f->ops->write) return MX_ENOSYS;
    if (f->flags == VFS_RDONLY) return MX_EACCES;
    if (!buf || !len) return 0;
    return f->ops->write(f, buf, len);
}

s64 vfs_seek(struct vfs_file *f, off_t off, int whence)
{
    if (!f || !f->ops || !f->ops->seek) return MX_ENOSYS;
    return f->ops->seek(f, off, whence);
}

/* -------------------------------------------------------
 * vfs_stat
 * -------------------------------------------------------*/
int vfs_stat(const char *path, struct vfs_stat *st)
{
    struct vfs_dentry *d = path_resolve(path);
    if (!d) return MX_ENOENT;
    if (!d->inode) { dentry_put(d); return MX_ENOENT; }
    int ret = MX_ENOSYS;
    if (d->inode->ops && d->inode->ops->stat)
        ret = d->inode->ops->stat(d->inode, st);
    dentry_put(d);
    return ret;
}

/* -------------------------------------------------------
 * vfs_mkdir / vfs_unlink / vfs_rmdir / vfs_rename
 * -------------------------------------------------------*/
int vfs_mkdir(const char *path, mode_t mode)
{
    char *buf; const char *pp, *base;
    int r = split_path(path, &buf, &pp, &base);
    if (r != MX_OK) return r;

    struct vfs_dentry *parent = path_resolve(pp);
    if (!parent) { kfree(buf); return MX_ENOENT; }

    int ret = MX_ENOSYS;
    if (parent->inode && parent->inode->ops && parent->inode->ops->mkdir)
        ret = parent->inode->ops->mkdir(parent, base, mode);

    dentry_put(parent);
    kfree(buf);
    return ret;
}

static int vfs_remove_common(const char *path, bool dir)
{
    char *buf; const char *pp, *base;
    int r = split_path(path, &buf, &pp, &base);
    if (r != MX_OK) return r;

    struct vfs_dentry *parent = path_resolve(pp);
    if (!parent) { kfree(buf); return MX_ENOENT; }

    int ret = MX_ENOSYS;
    if (parent->inode && parent->inode->ops) {
        if (!dir && parent->inode->ops->unlink)
            ret = parent->inode->ops->unlink(parent, base);
        else if (dir && parent->inode->ops->rmdir)
            ret = parent->inode->ops->rmdir(parent, base);
    }

    /* forget a cached dentry for the removed name */
    if (ret == MX_OK) {
        struct vfs_dentry *victim = NULL;
        spin_lock(&parent->lock);
        struct list_head *pos;
        list_for_each(pos, &parent->children) {
            struct vfs_dentry *c = list_entry(pos, struct vfs_dentry, child_node);
            if (strcmp(c->name, base) == 0) { victim = c; break; }
        }
        if (victim) list_del_init(&victim->child_node);
        spin_unlock(&parent->lock);
        if (victim) dentry_put(victim);       /* the dcache's reference */
    }

    dentry_put(parent);
    kfree(buf);
    return ret;
}

int vfs_unlink(const char *path) { return vfs_remove_common(path, false); }
int vfs_rmdir(const char *path)  { return vfs_remove_common(path, true);  }

int vfs_rename(const char *old_path, const char *new_path)
{
    if (!old_path || !new_path) return MX_EINVAL;

    char *obuf, *nbuf; const char *opp, *obase, *npp, *nbase;
    int r = split_path(old_path, &obuf, &opp, &obase);
    if (r != MX_OK) return r;
    r = split_path(new_path, &nbuf, &npp, &nbase);
    if (r != MX_OK) { kfree(obuf); return r; }

    int ret = MX_ENOENT;
    struct vfs_dentry *old_parent = path_resolve(opp);
    struct vfs_dentry *new_parent = old_parent ? path_resolve(npp) : NULL;

    if (old_parent && new_parent) {
        ret = MX_ENOSYS;
        if (old_parent->inode && old_parent->inode->ops &&
            old_parent->inode->ops->rename)
            ret = old_parent->inode->ops->rename(old_parent, obase,
                                                  new_parent, nbase);
    }
    if (old_parent) dentry_put(old_parent);
    if (new_parent) dentry_put(new_parent);
    kfree(obuf);
    kfree(nbuf);
    return ret;
}

/* -------------------------------------------------------
 * Directory iteration
 * -------------------------------------------------------*/
struct vfs_dir {
    struct vfs_file    *f;
    struct vfs_dentry  *dentry;
    struct list_head   *pos;
};

struct vfs_dir *vfs_opendir(const char *path)
{
    struct vfs_dentry *d = path_resolve(path);
    if (!d) return NULL;
    if (!d->inode || d->inode->type != INODE_DIR) {
        dentry_put(d);
        return NULL;
    }

    struct vfs_dir *dir = kzalloc(sizeof(*dir));
    if (!dir) { dentry_put(d); return NULL; }
    dir->dentry = d;

    /* Prefer the filesystem's own directory iterator: the dcache only holds
     * entries that were already looked up, so listing it returned a partial
     * (often empty) directory. */
    if (d->inode->ops && d->inode->ops->readdir) {
        struct vfs_file *f = kzalloc(sizeof(*f));
        if (!f) { dentry_put(d); kfree(dir); return NULL; }
        f->inode  = d->inode;
        f->dentry = d;               /* reference is owned by dir->dentry */
        f->ops    = d->inode->ops;
        f->pos    = 0;
        atomic_set(&f->refcount, 1);
        dir->f = f;
    } else {
        dir->pos = d->children.next;
    }
    return dir;
}

int vfs_readdir(struct vfs_dir *dir, struct vfs_dirent *de)
{
    if (!dir || !de) return MX_EINVAL;

    if (dir->f)
        return dir->f->ops->readdir(dir->f, de);   /* != MX_OK => end / error */

    spin_lock(&dir->dentry->lock);
    if (dir->pos == &dir->dentry->children) {
        spin_unlock(&dir->dentry->lock);
        return MX_ENOENT;
    }
    struct vfs_dentry *child = list_entry(dir->pos, struct vfs_dentry, child_node);
    strncpy(de->name, child->name, 255);
    de->name[255] = '\0';
    de->ino  = child->inode ? child->inode->ino : 0;
    de->type = child->inode ? child->inode->type : INODE_REG;
    dir->pos = dir->pos->next;
    spin_unlock(&dir->dentry->lock);
    return MX_OK;
}

void vfs_closedir(struct vfs_dir *dir)
{
    if (!dir) return;
    if (dir->f) kfree(dir->f);       /* no dentry ref of its own */
    if (dir->dentry) dentry_put(dir->dentry);
    kfree(dir);
}

/* -------------------------------------------------------
 * vfs_sync_all - flush all mounted filesystems
 * -------------------------------------------------------*/
int vfs_sync_all(void)
{
    spin_lock(&g_mount_lock);
    struct list_head *pos;
    list_for_each(pos, &g_mount_list) {
        struct vfs_mount *mnt = list_entry(pos, struct vfs_mount, node);
        if (mnt->ops && mnt->ops->sync) {
            /* Create a dummy file pointing at root */
            struct vfs_file f = {
                .inode  = mnt->root ? mnt->root->inode : NULL,
                .dentry = mnt->root,
                .flags  = 0,
                .ops    = mnt->ops,
            };
            mnt->ops->sync(&f);
        }
    }
    spin_unlock(&g_mount_lock);
    return MX_OK;
}

/* -------------------------------------------------------
 * Inode ref counting
 * -------------------------------------------------------*/
void vfs_inode_get(struct vfs_inode *inode)
{
    if (inode) atomic_inc_return(&inode->refcount);
}

void vfs_inode_put(struct vfs_inode *inode)
{
    if (!inode) return;
    if (atomic_dec_and_test(&inode->refcount)) {
        if (inode->ops && inode->ops->inode_put)
            inode->ops->inode_put(inode);
        kfree(inode);
    }
}
