/*
 * Protocol between the VFS (posix_server) and a file system server
 * (fs_ext2, and in the future fs_ufs, ramfs...).
 *
 * Each operation is a synchronous protected call (microkit_ppcall):
 *   MR0 = operation (FS_OP_*), MR1..MR4 = arguments
 * Reply:
 *   MR0 = result (>= 0) or -errno; MR1.. = extra results depending on the operation
 *
 * Paths and data travel through a shared buffer of FS_BUF_SIZE bytes. Paths
 * are absolute and already normalised by the VFS (no "." or "..").
 * When an operation takes two paths, the second one starts at FS_BUF_PATH2.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#define FS_BUF_SIZE   0x100000     /* 1 MiB, same as POSIX_BUF_SIZE */
#define FS_BUF_PATH2  0x8000
#define FS_PATH_MAX   4096

enum fs_op {
    /* buf: path. MR1: 1 = follow the last symbolic link.
     * -> inode; MR1 = mode (st_mode), MR2 = size, MR3 = owner uid, MR4 = gid */
    FS_OP_LOOKUP = 1,
    /* MR1: inode -> buf: Linux x86_64 struct stat (144 bytes) */
    FS_OP_STAT,
    /* MR1: inode, MR2: offset, MR3: bytes -> buf: data; result: bytes read */
    FS_OP_READ,
    /* MR1: inode, MR2: offset, MR3: bytes; buf: data -> bytes written */
    FS_OP_WRITE,
    /* MR1: inode, MR2: new size */
    FS_OP_TRUNCATE,
    /* MR1: directory inode, MR2: position, MR3: max bytes
     * -> buf: struct linux_dirent64[]; result: bytes; MR1 = next position */
    FS_OP_GETDENTS,
    /* MR1: inode -> buf: link target (no '\0'); result: length */
    FS_OP_READLINK,
    /* buf: path, MR1: mode (file type | permissions), MR2: owner uid, MR3: gid -> created inode */
    FS_OP_CREATE,
    /* buf: path, MR1: 1 = it is rmdir */
    FS_OP_UNLINK,
    /* buf: old path, buf+FS_BUF_PATH2: new path */
    FS_OP_RENAME,
    /* buf: link target, buf+FS_BUF_PATH2: path of the new link; MR1: owner uid, MR2: gid */
    FS_OP_SYMLINK,
    /* MR1: inode, MR2: new mode (permissions only) */
    FS_OP_CHMOD,
    /* -> buf: Linux x86_64 struct statfs (120 bytes) */
    FS_OP_STATFS,
    /* MR1: UNIX time at boot, MR2: TSC at that moment, MR3: TSC frequency */
    FS_OP_SETCLOCK,
    /* MR1: inode, MR2: atime, MR3: mtime (UNIX seconds; -1 = leave unchanged) */
    FS_OP_SETTIMES,
    /* MR1: inode, MR2: uid, MR3: gid (-1 = leave unchanged) */
    FS_OP_CHOWN,
    /* buf: existing path, buf+FS_BUF_PATH2: new name (hard link) */
    FS_OP_LINK,
    /* MR1: 1 = also unmount (shutdown): write everything, mark the file system clean, and refuse
     * further writes. 0 = sync only */
    FS_OP_SYNC,
    /* Who the next operations are for (path search permission is checked against it):
     * MR1: effective uid, MR2: effective gid, MR3: number of supplementary groups, MR4...: the
     * groups (in registers: the buffer may already hold the next operation's path). uid 0 is
     * not checked */
    FS_OP_SETCRED,
    /* -> buf: counters as text lines "name count cycles" (TSC cycles); result: length */
    FS_OP_STATS,
    /* The partition as a raw device (/dev/root, for e2fsprogs), through the block cache so the
     * file system sees what is written. MR1: FS_RAW_READ (-> buf) or FS_RAW_WRITE (buf ->; only
     * while the file system is read-only: -EBUSY otherwise), MR2: byte offset, MR3: bytes (up to
     * FS_BUF_SIZE) -> bytes done (0 at the end). MR1 = FS_RAW_SIZE -> size in bytes */
    FS_OP_RAW,
    /* MR1: 1 = read-write, 0 = read-only (written out and marked clean, as at unmount).
     * Read-write reads the superblock and group descriptors again (e2fsck may have changed them):
     * -EUCLEAN while the disk is not clean (e2fsck has not fixed it) */
    FS_OP_REMOUNT,
    /* -> MR1: flags: FS_STATE_RO (mounted read-only), FS_STATE_DIRTY (not cleanly unmounted:
     * mounted read-only until e2fsck has checked it) */
    FS_OP_STATE,
};

enum { FS_RAW_READ, FS_RAW_WRITE, FS_RAW_SIZE };
#define FS_STATE_RO    1
#define FS_STATE_DIRTY 2
