/* MONOX Kernel - MonoxFS Header */

#ifndef _MONOXFS_H
#define _MONOXFS_H

#include <kernel/types.h>
#include "../../fs/vfs/vfs.h"

extern struct fs_ops monoxfs_ops;

/* File extension types */
#define MONOXFS_EXT_RUNAP   ".Runap"
#define MONOXFS_EXT_CONFIG  ".Config"

/* Partition roles */
#define MONOXFS_ROLE_SYSTEM 0
#define MONOXFS_ROLE_USER   1

#endif /* _MONOXFS_H */
