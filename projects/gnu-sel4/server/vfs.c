/*
 * posix_server's VFS: file descriptors, current directory, paths and dispatch
 * of file calls to the file system server (fs_ext2, through the protocol in
 * include/sel4posix/fs_protocol.h). Pipes, eventfd/timerfd and sockets are in
 * vfs_*.c (see vfs.h).
 *
 * As in Unix, a descriptor points to an "open file description" (struct ofile)
 * that holds the offset; dup() shares the same one.
 *
 * Each process has its own descriptor table, current directory and umask
 * (struct proc); open file descriptions and pipes are global, so fork()
 * shares them between parent and child.
 *
 * For now there is a single file system mounted at "/" and paths are
 * normalised lexically ("a/../b" -> "b") before being sent.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vfs.h"

/* Buffer shared with the file server (set by Microkit, see meta.py) */
uintptr_t fs_buf;


struct ofile ofiles[NOFILE];

static struct ofile *fd_get(long fd)
{
    if (fd < 0 || fd >= NFD || cur->fdt[fd].of < 0) return NULL;
    return &ofiles[cur->fdt[fd].of];
}

int ofile_alloc(int type)
{
    for (int i = 0; i < NOFILE; i++) {
        if (!ofiles[i].refs) {
            ofiles[i].refs = 1;
            ofiles[i].type = type;
            ofiles[i].ino = 0;
            ofiles[i].off = 0;
            ofiles[i].flags = 0;
            ofiles[i].path[0] = 0;
            return i;
        }
    }
    return -1;
}

int fd_alloc(int of, long min, bool cloexec)
{
    for (long fd = min < 0 ? 0 : min; fd < NFD; fd++) {
        if (cur->fdt[fd].of < 0) {
            cur->fdt[fd].of = of;
            cur->fdt[fd].cloexec = cloexec;
            return fd;
        }
    }
    return -EMFILE;
}

/* Drops a reference to an open file description; when it is freed, pipes
 * wake up the other end (EOF for readers, EPIPE for writers) */
void ofile_release(int of)
{
    struct ofile *f = &ofiles[of];
    if (--f->refs > 0) return;
    if (f->type == OF_PIPE_R || f->type == OF_PIPE_W) {
        struct pipe *pp = &pipes[f->ino];
        if (f->type == OF_PIPE_R) pp->readers--; else pp->writers--;
        if (f->type == OF_PIPE_R && (f->flags & O_ACCMODE) == O_RDWR) pp->writers--;   /* FIFO opened O_RDWR */
        proc_wake_mask(pp->rwait | pp->wwait | pp->owait);
        pp->rwait = pp->wwait = pp->owait = 0;
        pipe_maybe_free(pp);
    }
    if (f->type == OF_LINUX) bridge_close(f->ino);
    if (f->type == OF_SOCKET) net_call(NET_OP_CLOSE, f->ino, 0, 0, 0);
    if (f->type == OF_EVENTFD) evfds[f->ino].used = false;
    if (f->type == OF_TIMERFD) { tfds[f->ino].used = false; timer_rearm(); }
    int type = f->type;
    f->type = OF_FREE;
    if (type == OF_USOCK) usock_free(f->ino);           /* it may release descriptors in transit */
    if (type == OF_SHM) mem_shm_unref(f->ino);
}

long fd_close(long fd)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    ofile_release(cur->fdt[fd].of);
    cur->fdt[fd].of = -1;
    return 0;
}

/* ---------------- file server ---------------- */

/* fs_ext2 checks the search permission of the directories on a path against the caller: it is
 * told who that is whenever it changes. The server's own work (writing /etc/resolv.conf from a
 * notification, when `cur` is whoever ran last) is done as root. */
static bool fs_as_server;
static uint32_t fs_cred_uid, fs_cred_gid, fs_cred_n, fs_cred_groups[NGROUPS_];

static void fs_send_cred(void)
{
    uint32_t uid = 0, gid = 0, n = 0;
    const uint32_t *g = fs_cred_groups;
    if (!fs_as_server && cur) { uid = cur->euid; gid = cur->egid; n = cur->ngroups; g = cur->groups; }
    if (uid == fs_cred_uid && gid == fs_cred_gid && n == fs_cred_n && !memcmp(g, fs_cred_groups, n * 4)) return;
    microkit_mr_set(0, FS_OP_SETCRED);
    microkit_mr_set(1, uid);
    microkit_mr_set(2, gid);
    microkit_mr_set(3, n);
    for (uint32_t i = 0; i < n; i++) microkit_mr_set(4 + i, g[i]);
    microkit_ppcall(FS_CH, microkit_msginfo_new(0, 4 + n));
    fs_cred_uid = uid; fs_cred_gid = gid; fs_cred_n = n;
    memmove(fs_cred_groups, g, n * 4);
}

long fs_call(long op, uint64_t a1, uint64_t a2, uint64_t a3)
{
    uint64_t t0 = clock_rdtsc();
    fs_send_cred();
    microkit_mr_set(0, op);
    microkit_mr_set(1, a1);
    microkit_mr_set(2, a2);
    microkit_mr_set(3, a3);
    microkit_ppcall(FS_CH, microkit_msginfo_new(0, 4));
    long r = (long)microkit_mr_get(0);
    perf_add(&perf_fs[op & 31], 1, t0);
    return r;
}

size_t vfs_fs_stats(char *dst, size_t max)
{
    long r = fs_call(FS_OP_STATS, 0, 0, 0);
    if (r <= 0) return 0;
    if ((size_t)r > max) r = max;
    memcpy(dst, (void *)fs_buf, r);
    return r;
}

void fs_set_path(uint32_t off, const char *abs)
{
    size_t n = strlen(abs);
    memcpy((char *)fs_buf + off, abs, n + 1);
}

/* Looks up an absolute path. Returns the inode and, optionally, mode and size. */
uint32_t last_uid, last_gid;

long fs_lookup(const char *abs, bool follow, uint32_t *mode, uint64_t *size)
{
    fs_set_path(0, abs);
    long ino = fs_call(FS_OP_LOOKUP, follow, 0, 0);
    if (ino >= 0) {
        if (mode) *mode = microkit_mr_get(1);
        if (size) *size = microkit_mr_get(2);
        last_uid = microkit_mr_get(3);
        last_gid = microkit_mr_get(4);
    }
    return ino;
}

/* Replaces a whole file from the server */
long write_file(const char *abs, const char *data, size_t len)
{
    fs_as_server = true;                               /* the server's own write: as root */
    long ino = fs_lookup(abs, true, NULL, NULL);
    if (ino == -ENOENT) {
        fs_set_path(0, abs);
        ino = fs_call(FS_OP_CREATE, S_IFREG | 0644, 0, 0);
    }
    if (ino > 0) proc_image_release(ino);
    long r = ino < 0 ? ino : fs_call(FS_OP_TRUNCATE, ino, 0, 0);
    if (r >= 0) {
        memcpy((void *)fs_buf, data, len);
        r = fs_call(FS_OP_WRITE, ino, 0, len);
    }
    fs_as_server = false;
    return r;
}

/* ---------------- permissions ---------------- */

/* The directory that contains abs (abs is normalised, not "/") */
static void parent_of(const char *abs, char *out)
{
    size_t n = strlen(abs);
    while (n > 1 && abs[n - 1] != '/') n--;
    if (n > 1) n--;                                    /* drop the slash, except for "/" itself */
    memcpy(out, abs, n);
    out[n] = 0;
}

/* Write and search permission on the directory that will hold a new name (or lose one) */
long may_change_dir(const char *abs)
{
    char dir[FS_PATH_MAX];
    parent_of(abs, dir);
    uint32_t mode;
    long ino = fs_lookup(dir, true, &mode, NULL);
    if (ino < 0) return ino;
    return cred_may(cur, mode, last_uid, last_gid, 3) ? 0 : -EACCES;
}

/* Removing or renaming abs: as above, plus the sticky bit (/tmp): then only the file's owner,
 * the directory's owner or root */
static long may_remove(const char *abs)
{
    char dir[FS_PATH_MAX];
    parent_of(abs, dir);
    uint32_t dmode;
    long d = fs_lookup(dir, true, &dmode, NULL);
    if (d < 0) return d;
    uint32_t duid = last_uid;
    if (!cred_may(cur, dmode, duid, last_gid, 3)) return -EACCES;
    if (!(dmode & 01000) || cur->euid == 0 || cur->euid == duid) return 0;
    long f = fs_lookup(abs, false, NULL, NULL);
    if (f < 0) return f;
    return last_uid == cur->euid ? 0 : -EPERM;
}

/* Changing the mode, owner or times of a file: only its owner or root */
static long may_own(uint32_t uid) { return cur->euid == 0 || cur->euid == uid ? 0 : -EPERM; }


static long fs_size(uint32_t ino, uint64_t *size)
{
    long r = fs_call(FS_OP_STAT, ino, 0, 0);
    if (r >= 0) memcpy(size, (char *)fs_buf + ST_SIZE_OFFSET, 8);
    return r;
}

/* ---------------- paths ---------------- */

static const char *client_path(uint32_t off)
{
    char *p = (char *)client_buf + off;
    p[FS_PATH_MAX - 1] = 0;
    return p;
}

/* Physical resolution of an absolute path, as realpath(): every symlink is followed, so ".."
 * goes to the real parent. A component that does not exist ends the resolution: the rest is
 * kept lexically (create, mkdir...). */
static long realpath_abs(const char *in, char *out)
{
    static char rest[FS_PATH_MAX * 2], res[FS_PATH_MAX], tmp[FS_PATH_MAX * 2];
    size_t inl = strlen(in);
    if (inl >= sizeof rest) return -ENAMETOOLONG;
    memcpy(rest, in, inl + 1);
    size_t rl = 0, pos = 0;
    int loops = 0;
    bool missing = false;
    while (rest[pos]) {
        while (rest[pos] == '/') pos++;
        if (!rest[pos]) break;
        size_t e = pos;
        while (rest[e] && rest[e] != '/') e++;
        size_t len = e - pos;
        if (len == 1 && rest[pos] == '.') { pos = e; continue; }
        if (len == 2 && rest[pos] == '.' && rest[pos + 1] == '.') {
            while (rl > 0 && res[rl - 1] != '/') rl--;
            if (rl > 0) rl--;
            pos = e;
            continue;
        }
        if (rl + 1 + len + 1 > FS_PATH_MAX) return -ENAMETOOLONG;
        size_t prev = rl;
        res[rl++] = '/';
        memcpy(res + rl, rest + pos, len);
        rl += len;
        res[rl] = 0;
        pos = e;
        if (missing) continue;
        uint32_t mode;
        long ino = fs_lookup(res, false, &mode, NULL);
        if (ino == -ENOENT) { missing = true; continue; }
        if (ino < 0) return ino;
        if (!S_ISLNK(mode)) continue;
        if (++loops > 40) return -ELOOP;
        long n = fs_call(FS_OP_READLINK, ino, 0, 0);
        if (n < 0) return n;
        size_t restl = strlen(rest + pos);
        if ((size_t)n + 1 + restl + 1 > sizeof tmp) return -ENAMETOOLONG;
        memcpy(tmp, (void *)fs_buf, n);                 /* target + what was left after the link */
        tmp[n] = '/';
        memcpy(tmp + n + 1, rest + pos, restl + 1);
        memcpy(rest, tmp, n + 1 + restl + 1);
        pos = 0;
        rl = tmp[0] == '/' ? 0 : prev;                 /* absolute target: start again at the root */
    }
    if (rl == 0) res[rl++] = '/';
    res[rl] = 0;
    memcpy(out, res, rl + 1);
    return 0;
}

/* Turns path (relative to dirfd or the current directory) into a normalised absolute path */
long normalize(long dirfd, const char *path, char *out)
{
    static char tmp[FS_PATH_MAX * 2];
    if (!*path) return -ENOENT;
    const char *base = "";
    if (path[0] != '/') {
        if (dirfd == AT_FDCWD) {
            base = cur->cwd;
        } else {
            struct ofile *f = fd_get(dirfd);
            if (!f) return -EBADF;
            if (f->type != OF_DIR && f->type != OF_PROCDIR) return -ENOTDIR;
            base = f->path;
        }
    }
    size_t bl = strlen(base), pl = strlen(path);
    if (bl + pl + 2 > sizeof(tmp)) return -ENAMETOOLONG;
    memcpy(tmp, base, bl);
    tmp[bl] = '/';
    memcpy(tmp + bl + 1, path, pl + 1);

    size_t o = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = e - p;
        if (len == 1 && p[0] == '.') {
            /* nothing */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;    /* drop the last component */
            if (o > 0) o--;
        } else {
            if (o + 1 + len + 1 > FS_PATH_MAX) return -ENAMETOOLONG;
            out[o++] = '/';
            memcpy(out + o, p, len);
            o += len;
        }
        p = e;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    /* ".." after a symlink means the link target's parent (POSIX): resolve physically. Only
     * when ".." is there, so the usual case stays a lexical, lookup-free operation */
    for (const char *q = tmp; *q; q++)
        if (q[0] == '/' && q[1] == '.' && q[2] == '.' && (q[3] == '/' || q[3] == 0)) return realpath_abs(tmp, out);
    return 0;
}

/* Devices that live in the server rather than on disk */
static int dev_type(const char *abs)
{
    if (!strcmp(abs, "/dev/tty") || !strcmp(abs, "/dev/console")) return OF_TTY;
    if (!strcmp(abs, "/dev/null")) return OF_NULL;
    if (!strcmp(abs, "/dev/linux")) return OF_LINUX;   /* session with the Linux VM */
    if (!strcmp(abs, "/dev/kmsg")) return OF_KMSG;     /* messages to the serial port */
    if (!strcmp(abs, "/dev/urandom") || !strcmp(abs, "/dev/random")) return OF_RANDOM;
    return OF_FREE;
}

/* struct stat for the server's character devices */
static void dev_stat(int type)
{
    uint64_t *st = (uint64_t *)client_buf;
    memset(st, 0, K_STAT_SIZE);
    st[0] = 5;                                                 /* st_dev */
    st[1] = type == OF_TTY ? 1 : 2;                            /* st_ino */
    st[2] = 1;                                                 /* st_nlink */
    ((uint32_t *)client_buf)[6] = S_IFCHR | (type == OF_TTY ? 0620 : 0666);   /* st_mode */
    st[5] = type == OF_TTY ? 0x0500 : 0x0103;                  /* st_rdev */
    st[7] = 1024;                                              /* st_blksize */
}

/* ---------------- calls ---------------- */

static long do_open(long dirfd, uint32_t flags, uint32_t mode)
{
    char abs[FS_PATH_MAX];
    long r = normalize(dirfd, client_path(0), abs);
    if (r) return r;

    int type = dev_type(abs);
    uint32_t ino = 0;
    if (type == OF_FREE && is_proc(abs)) {
        int k = procfs_kind(abs);
        if (k == PROCFS_LINK) {                       /* /proc/self: open /proc/<pid> */
            char t[32];
            long n = procfs_readlink(abs, t, sizeof t - 1);
            if (n < 0 || t[0] == '/') return -ENOENT;
            t[n] = 0;
            memcpy(abs, "/proc/", 6);
            memcpy(abs + 6, t, n + 1);
            k = procfs_kind(abs);
        }
        if (k == PROCFS_NONE) return (flags & O_CREAT) ? -EACCES : -ENOENT;
        if ((flags & O_ACCMODE) != O_RDONLY) return k == PROCFS_DIR ? -EISDIR : -EACCES;
        if (k == PROCFS_FILE && (flags & O_DIRECTORY)) return -ENOTDIR;
        type = k == PROCFS_DIR ? OF_PROCDIR : OF_PROCFILE;
    }
    if (type == OF_FREE) {
        uint32_t fmode;
        uint64_t size;
        bool created = false;
        long res = fs_lookup(abs, !(flags & O_NOFOLLOW), &fmode, &size);
        if (res == -ENOENT && (flags & O_CREAT)) {
            long p = may_change_dir(abs);
            if (p) return p;
            fs_set_path(0, abs);
            res = fs_call(FS_OP_CREATE, S_IFREG | (mode & 07777 & ~cur->umask), cur->euid, cur->egid);
            fmode = S_IFREG;
            size = 0;
            created = true;
        } else if (res >= 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
            return -EEXIST;
        }
        if (res < 0) return res;
        ino = res;
        if (S_ISLNK(fmode)) return -ELOOP;
        if (!created) {                                /* read/write permission on what exists */
            uint32_t acc = flags & O_ACCMODE;
            int want = (acc != O_WRONLY ? 4 : 0) | (acc != O_RDONLY || (flags & O_TRUNC) ? 2 : 0);
            if (!cred_may(cur, fmode, last_uid, last_gid, want)) return -EACCES;
        }
        if ((fmode & S_IFMT) == S_IFIFO) return fifo_open(ino, flags, abs);
        if ((fmode & S_IFMT) == S_IFSOCK_) return -ENXIO;          /* sockets are not opened */
        if (S_ISDIR(fmode)) {
            if ((flags & O_ACCMODE) != O_RDONLY) return -EISDIR;
            type = OF_DIR;
        } else {
            if (flags & O_DIRECTORY) return -ENOTDIR;
            type = OF_FILE;
            if ((flags & O_TRUNC) && S_ISREG(fmode) && (flags & O_ACCMODE) != O_RDONLY && !created && size) {
                proc_image_release(ino);
                res = fs_call(FS_OP_TRUNCATE, ino, 0, 0);
                if (res < 0) return res;
            }
        }
    }
    if (type == OF_LINUX) {
        long sid = bridge_open();
        if (sid < 0) return sid;
        ino = sid;
    }
    int of = ofile_alloc(type);
    if (of < 0) {
        if (type == OF_LINUX) bridge_close(ino);
        return -EMFILE;
    }
    ofiles[of].ino = ino;
    ofiles[of].flags = flags;
    memcpy(ofiles[of].path, abs, strlen(abs) + 1);
    int fd = fd_alloc(of, 0, flags & O_CLOEXEC);
    if (fd < 0) ofiles[of].refs = 0;
    return fd;
}


static long do_read(long fd, size_t len, bool positional, uint64_t pos)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (f->type) {
    case OF_TTY:    return positional ? -ESPIPE : tty_read(len);
    case OF_NULL:   return 0;
    case OF_DIR:    return -EISDIR;
    case OF_PIPE_R: return positional ? -ESPIPE : pipe_read(f, len);
    case OF_PIPE_W: return -EBADF;
    case OF_LINUX:  return bridge_read(f->ino, (uint8_t *)client_buf, len > POSIX_BUF_SIZE ? POSIX_BUF_SIZE : len);
    case OF_SOCKET: return positional ? -ESPIPE : sock_recv(f, len, 0);
    case OF_USOCK:   return positional ? -ESPIPE : usock_recv(f, len, 0, 0);
    case OF_KMSG:    return 0;
    case OF_RANDOM: {
        if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
        for (size_t i = 0; i < len; i += 8) {
            unsigned long long v = 0;
            for (int k = 0; k < 10; k++) {
                unsigned char ok;
                __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
                if (ok) break;
                v ^= clock_rdtsc() * 0x9e3779b97f4a7c15ULL;
            }
            memcpy((char *)client_buf + i, &v, len - i < 8 ? len - i : 8);
        }
        return len;
    }
    case OF_SHM: {
        if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
        long r = mem_shm_rw(f->ino, positional ? pos : f->off, (void *)client_buf, len, false);
        if (r > 0 && !positional) f->off += r;
        return r;
    }
    case OF_EVENTFD: return evfd_read(f, len);
    case OF_TIMERFD: return tfd_read(f, len);
    case OF_EPOLL:   return -EINVAL;
    case OF_PROCDIR: return -EISDIR;
    case OF_PROCFILE: {
        if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
        long r = procfs_read(f->path, positional ? pos : f->off, (char *)client_buf, len);
        if (r > 0 && !positional) f->off += r;
        return r;
    }
    }
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    long r = fs_call(FS_OP_READ, f->ino, positional ? pos : f->off, len);
    if (r > 0) {
        memcpy((void *)client_buf, (void *)fs_buf, r);
        if (!positional) f->off += r;
    }
    return r;
}

static long do_write(long fd, size_t len, bool positional, uint64_t pos)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (f->type) {
    case OF_TTY:    return positional ? -ESPIPE : tty_write((const char *)client_buf, len);
    case OF_NULL:   return len;
    case OF_DIR:    return -EISDIR;
    case OF_PIPE_W: return positional ? -ESPIPE : pipe_write(f, len);
    case OF_PIPE_R:                                    /* a FIFO opened O_RDWR also writes */
        if ((f->flags & O_ACCMODE) != O_RDWR) return -EBADF;
        return positional ? -ESPIPE : pipe_write(f, len);
    case OF_LINUX:  return bridge_write(f->ino, (const uint8_t *)client_buf, len);
    case OF_SOCKET: return positional ? -ESPIPE : sock_send(f, len, 0, 0);
    case OF_PROCDIR: case OF_PROCFILE: return -EBADF;
    case OF_USOCK:   return positional ? -ESPIPE : usock_send(f, len, 0, 0);
    case OF_RANDOM:  return len;                       /* writing "entropy": accepted */
    case OF_KMSG: {                                    /* to the serial port (kernel debug output) */
        static char line[257];
        size_t n = len < 256 ? len : 256;
        memcpy(line, (const void *)client_buf, n);
        line[n] = 0;
        microkit_dbg_puts(line);
        return n;
    }
    case OF_SHM: {                                     /* like a file: it grows when written */
        if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
        uint64_t off = positional ? pos : f->off;
        if (off + len > mem_shm_size(f->ino)) {
            long t = mem_shm_truncate(f->ino, off + len);
            if (t < 0) return t;
        }
        long r = mem_shm_rw(f->ino, off, (void *)client_buf, len, true);
        if (r > 0 && !positional) f->off = off + r;
        return r;
    }
    case OF_EVENTFD: return evfd_write(f, len);
    case OF_TIMERFD: case OF_EPOLL: return -EINVAL;
    }
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    uint64_t off = positional ? pos : f->off;
    if ((f->flags & O_APPEND) && !positional) {
        long r = fs_size(f->ino, &off);
        if (r < 0) return r;
    }
    proc_image_release(f->ino);                   /* before fs_buf gets the data */
    memcpy((void *)fs_buf, (void *)client_buf, len);
    long r = fs_call(FS_OP_WRITE, f->ino, off, len);
    if (r > 0 && !positional) f->off = off + r;
    return r;
}

static long do_lseek(long fd, int64_t off, int whence)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->type == OF_NULL) return 0;
    if (f->type == OF_SHM) {
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->off : whence == 2 ? (int64_t)mem_shm_size(f->ino) : -1;
        if (base < 0 || base + off < 0) return -EINVAL;
        return f->off = base + off;
    }
    if (f->type == OF_PROCFILE || f->type == OF_PROCDIR) {   /* generated contents: only from the start */
        if (whence == 0 && off >= 0) return f->off = off;
        if (whence == 1 && (int64_t)f->off + off >= 0) return f->off += off;
        return -EINVAL;
    }
    if (f->type != OF_FILE && f->type != OF_DIR) return -ESPIPE;
    int64_t base;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = f->off; break;
    case 2: {
        uint64_t size;
        long r = fs_size(f->ino, &size);
        if (r < 0) return r;
        base = size;
        break;
    }
    default: return -EINVAL;
    }
    if (base + off < 0) return -EINVAL;
    f->off = base + off;
    return f->off;
}

static long stat_ino(uint32_t ino)
{
    long r = fs_call(FS_OP_STAT, ino, 0, 0);
    if (r >= 0) memcpy((void *)client_buf, (void *)fs_buf, K_STAT_SIZE);
    return r;
}

static long do_fstat(long fd)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->type == OF_PROCDIR || f->type == OF_PROCFILE) return procfs_stat(f->path, (void *)client_buf);
    if (f->type == OF_SHM) {
        memset((void *)client_buf, 0, K_STAT_SIZE);
        ((uint32_t *)client_buf)[6] = S_IFREG | 0600;
        ((uint64_t *)client_buf)[2] = 1;
        ((uint64_t *)client_buf)[ST_SIZE_OFFSET / 8] = mem_shm_size(f->ino);
        ((uint64_t *)client_buf)[7] = 4096;
        return 0;
    }
    if (f->type == OF_EVENTFD || f->type == OF_TIMERFD || f->type == OF_EPOLL) {   /* "anon_inode" */
        memset((void *)client_buf, 0, K_STAT_SIZE);
        ((uint32_t *)client_buf)[6] = 0600;
        ((uint64_t *)client_buf)[2] = 1;
        return 0;
    }
    if (f->type == OF_SOCKET || f->type == OF_USOCK) {
        memset((void *)client_buf, 0, K_STAT_SIZE);
        ((uint32_t *)client_buf)[6] = 0140777;                     /* S_IFSOCK */
        return 0;
    }
    if (f->type == OF_TTY || f->type == OF_NULL || f->type == OF_LINUX || f->type == OF_KMSG || f->type == OF_RANDOM) {
        dev_stat(f->type == OF_TTY ? OF_TTY : OF_NULL);
        return 0;
    }
    if (f->type == OF_PIPE_R || f->type == OF_PIPE_W) {
        memset((void *)client_buf, 0, K_STAT_SIZE);
        ((uint32_t *)client_buf)[6] = 0010600;            /* S_IFIFO | 0600 */
        ((uint64_t *)client_buf)[2] = 1;                  /* st_nlink */
        ((uint64_t *)client_buf)[7] = PIPE_SIZE;          /* st_blksize */
        return 0;
    }
    return stat_ino(f->ino);
}

static long do_stat(long dirfd, long flags)
{
    const char *path = client_path(0);
    if (!*path && (flags & AT_EMPTY_PATH)) return do_fstat(dirfd);
    char abs[FS_PATH_MAX];
    long r = normalize(dirfd, path, abs);
    if (r) return r;
    int type = dev_type(abs);
    if (type != OF_FREE) {
        dev_stat(type);
        return 0;
    }
    if (is_proc(abs)) {
        if (!(flags & AT_SYMLINK_NOFOLLOW) && procfs_kind(abs) == PROCFS_LINK) {
            char t[FS_PATH_MAX];
            long n = procfs_readlink(abs, t, sizeof t - 1);
            if (n < 0) return n;
            t[n] = 0;
            if (t[0] == '/') {                        /* exe, cwd: the real file */
                long ino = fs_lookup(t, true, NULL, NULL);
                return ino < 0 ? ino : stat_ino(ino);
            }
            memcpy(abs, "/proc/", 6);                 /* self */
            memcpy(abs + 6, t, n + 1);
        }
        return procfs_stat(abs, (void *)client_buf);
    }
    long ino = fs_lookup(abs, !(flags & AT_SYMLINK_NOFOLLOW), NULL, NULL);
    if (ino < 0) return ino;
    return stat_ino(ino);
}

static long do_getdents(long fd, size_t len)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    if (f->type == OF_PROCDIR) return procfs_getdents(f->path, &f->off, (char *)client_buf, len);
    if (f->type != OF_DIR) return -ENOTDIR;
    long r = fs_call(FS_OP_GETDENTS, f->ino, f->off, len);
    if (r >= 0) {
        f->off = microkit_mr_get(1);
        memcpy((void *)client_buf, (void *)fs_buf, r);
    }
    return r;
}

static long do_chdir(const char *abs)
{
    if (is_proc(abs)) {
        if (procfs_kind(abs) != PROCFS_DIR) return -ENOTDIR;
        memcpy(cur->cwd, abs, strlen(abs) + 1);
        return 0;
    }
    uint32_t mode;
    long ino = fs_lookup(abs, true, &mode, NULL);
    if (ino < 0) return ino;
    if (!S_ISDIR(mode)) return -ENOTDIR;
    if (!cred_may(cur, mode, last_uid, last_gid, 1)) return -EACCES;
    return realpath_abs(abs, cur->cwd);                /* the physical directory, as getcwd() shows */
}

/* Calls that take a path: normalise it and operate on it */
static long path_op(long nr, long dirfd, long a, long b)
{
    char abs[FS_PATH_MAX];
    long r = normalize(dirfd, client_path(0), abs);
    if (r) return r;
    uint32_t mode;

    switch (nr) {
    case __NR_chdir:
        return do_chdir(abs);
    case __NR_mknod:                                   /* only FIFOs (and plain files): no device nodes */
        if ((a & S_IFMT) == S_IFIFO || (a & S_IFMT) == S_IFREG || !(a & S_IFMT)) {
            if (dev_type(abs) != OF_FREE || is_proc(abs)) return -EEXIST;
            if ((r = may_change_dir(abs))) return r;
            fs_set_path(0, abs);
            r = fs_call(FS_OP_CREATE, ((a & S_IFMT) ? (a & S_IFMT) : S_IFREG) | (a & 07777 & ~cur->umask), cur->euid, cur->egid);
            return r < 0 ? r : 0;
        }
        return -EPERM;
    case __NR_mkdir:
        if ((r = may_change_dir(abs))) return r;
        fs_set_path(0, abs);
        r = fs_call(FS_OP_CREATE, S_IFDIR | (a & 07777 & ~cur->umask), cur->euid, cur->egid);
        return r < 0 ? r : 0;
    case __NR_unlink:
        if (dev_type(abs) != OF_FREE) return -EBUSY;
        if ((r = may_remove(abs))) return r;
        if (!a) {                                      /* a program running from it keeps its pages */
            long ino = fs_lookup(abs, false, NULL, NULL);
            if (ino > 0) proc_image_release(ino);
        }
        fs_set_path(0, abs);
        return fs_call(FS_OP_UNLINK, a, 0, 0);       /* a: 1 = rmdir */
    case __NR_readlink: {
        if (is_proc(abs)) return procfs_readlink(abs, (char *)client_buf, a);
        long ino = fs_lookup(abs, false, &mode, NULL);
        if (ino < 0) return ino;
        if (!S_ISLNK(mode)) return -EINVAL;
        r = fs_call(FS_OP_READLINK, ino, 0, 0);
        if (r < 0) return r;
        if ((size_t)r > (size_t)a) r = a;
        memcpy((void *)client_buf, (void *)fs_buf, r);
        return r;
    }
    case __NR_truncate: {
        long ino = fs_lookup(abs, true, &mode, NULL);
        if (ino < 0) return ino;
        if (S_ISDIR(mode)) return -EISDIR;
        if (!cred_may(cur, mode, last_uid, last_gid, 2)) return -EACCES;
        proc_image_release(ino);
        return fs_call(FS_OP_TRUNCATE, ino, a, 0);
    }
    case __NR_access: {
        if (dev_type(abs) != OF_FREE) return 0;
        if (is_proc(abs)) return procfs_kind(abs) == PROCFS_NONE ? -ENOENT : 0;
        long ino = fs_lookup(abs, true, &mode, NULL);
        if (ino < 0) return ino;
        /* with the real IDs (F_OK = 0: it exists) */
        return cred_may_real(cur, mode, last_uid, last_gid, a & 7) ? 0 : -EACCES;
    }
    case __NR_chmod: {
        long ino = fs_lookup(abs, true, NULL, NULL);
        if (ino < 0) return ino;
        if ((r = may_own(last_uid))) return r;
        return fs_call(FS_OP_CHMOD, ino, a, 0);
    }
    case __NR_statfs:
        r = fs_call(FS_OP_STATFS, 0, 0, 0);
        if (r >= 0) memcpy((void *)client_buf, (void *)fs_buf, K_STATFS_SIZE);
        return r;
    }
    return -ENOSYS;
}

/* rename and symlink: two paths */
static long do_rename(long olddirfd, long newdirfd)
{
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    long r = normalize(olddirfd, client_path(0), from);
    if (r) return r;
    r = normalize(newdirfd, client_path(POSIX_BUF_PATH2), to);
    if (r) return r;
    if ((r = may_remove(from))) return r;
    long tino = fs_lookup(to, false, NULL, NULL);
    r = tino >= 0 ? may_remove(to) : may_change_dir(to);
    if (r) return r;
    if (tino > 0) proc_image_release(tino);         /* the file it replaces */
    fs_set_path(0, from);
    fs_set_path(FS_BUF_PATH2, to);
    return fs_call(FS_OP_RENAME, 0, 0, 0);
}

/* link/linkat: hard link */
static long do_link(long olddirfd, long newdirfd)
{
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    long r = normalize(olddirfd, client_path(0), from);
    if (r) return r;
    r = normalize(newdirfd, client_path(POSIX_BUF_PATH2), to);
    if (r) return r;
    if (dev_type(from) != OF_FREE || is_proc(from) || is_proc(to)) return -EXDEV;
    if ((r = may_change_dir(to))) return r;
    fs_set_path(0, from);
    fs_set_path(FS_BUF_PATH2, to);
    return fs_call(FS_OP_LINK, 0, 0, 0);
}

static long do_symlink(long newdirfd)
{
    char link[FS_PATH_MAX];
    const char *target = client_path(0);          /* the target is stored as is */
    if (!*target) return -ENOENT;
    long r = normalize(newdirfd, client_path(POSIX_BUF_PATH2), link);
    if (r) return r;
    static char saved_target[FS_PATH_MAX];               /* may_change_dir reuses the buffers */
    memcpy(saved_target, target, strlen(target) + 1);
    if ((r = may_change_dir(link))) return r;
    target = saved_target;
    fs_set_path(0, target);
    fs_set_path(FS_BUF_PATH2, link);
    return fs_call(FS_OP_SYMLINK, cur->euid, cur->egid, 0);
}



static short fd_poll(int fd, short events, bool wait)
{
    struct ofile *f = fd_get(fd);
    if (!f) return POLLNVAL;
    bool in = events & POLLIN, out = events & POLLOUT;
    short r = 0;
    switch (f->type) {
    case OF_TTY: {
        int t = tty_poll(in, out, wait);
        if (t & 1) r |= POLLIN;
        if (t & 4) r |= POLLOUT;
        break;
    }
    case OF_PIPE_R: {
        struct pipe *pp = &pipes[f->ino];
        if (pp->tail != pp->head) r |= POLLIN;
        if (!pp->writers) r |= POLLHUP;
        if ((f->flags & O_ACCMODE) == O_RDWR && pp->tail - pp->head < PIPE_SIZE) r |= POLLOUT;
        if (!r && wait && in) pp->rwait |= WAIT_BIT(cur);
        break;
    }
    case OF_PIPE_W: {
        struct pipe *pp = &pipes[f->ino];
        if (!pp->readers) r |= POLLERR;
        else if (pp->tail - pp->head < PIPE_SIZE) r |= POLLOUT;
        if (!r && wait && out) pp->wwait |= WAIT_BIT(cur);
        break;
    }
    case OF_LINUX: {
        int b = bridge_poll(f->ino, in, out, wait);
        if (b & 1) r |= POLLIN;
        if (b & 4) r |= POLLOUT;
        break;
    }
    case OF_EVENTFD:
        r = evfd_poll(f, events, in, out, wait);
        break;
    case OF_TIMERFD:
        r = tfd_poll(f, in, wait);
        break;
    case OF_EPOLL:                         /* epoll nested in poll: never ready (not implemented) */
        break;
    case OF_USOCK:
        r = usock_poll(f, events, wait);
        break;
    case OF_SOCKET:
        r = sock_poll(f, events, wait);
        break;
    default:                               /* files, directories, /dev/null: always ready */
        r = POLLIN | POLLOUT;
    }
    return r & (events | POLLERR | POLLHUP | POLLNVAL);
}

static long do_poll(long nfds, long timeout_ns)
{
    if (nfds < 0 || nfds * sizeof(struct k_pollfd) > POSIX_BUF_SIZE) return -EINVAL;
    struct k_pollfd *pf = (struct k_pollfd *)client_buf;
    long ready = 0;
    for (long i = 0; i < nfds; i++) {
        pf[i].revents = pf[i].fd < 0 ? 0 : fd_poll(pf[i].fd, pf[i].events, false);
        if (pf[i].revents) ready++;
    }
    if (ready || timeout_ns == 0) return ready;
    for (long i = 0; i < nfds; i++)                        /* register to be woken up */
        if (pf[i].fd >= 0) fd_poll(pf[i].fd, pf[i].events, true);
    if (timeout_ns > 0) timer_wait(cur, timeout_ns);
    return POSIX_RETRY;
}

/* utimensat(): buf = path (empty = dirfd itself); buf+PATH2 = timespec[2] if a3 != 0 */
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
static long do_utimensat(long dirfd, long have_times, long flags)
{
    uint32_t ino;
    const char *path = client_path(0);
    if (!*path) {
        struct ofile *f = fd_get(dirfd);
        if (!f) return -EBADF;
        if (f->type != OF_FILE && f->type != OF_DIR) return -EINVAL;
        ino = f->ino;
    } else {
        char abs[FS_PATH_MAX];
        long r = normalize(dirfd, path, abs);
        if (r) return r;
        if (dev_type(abs) != OF_FREE) return 0;
        long i = fs_lookup(abs, !(flags & AT_SYMLINK_NOFOLLOW), NULL, NULL);
        if (i < 0) return i;
        ino = i;
    }
    int64_t t[2] = { (int64_t)clock_now(), (int64_t)clock_now() };
    bool explicit_time = false;
    if (have_times) {
        const int64_t *ts = (const int64_t *)(client_buf + POSIX_BUF_PATH2);   /* {sec, nsec} x 2 */
        for (int k = 0; k < 2; k++) {
            if (ts[2 * k + 1] == UTIME_OMIT) t[k] = -1;
            else if (ts[2 * k + 1] != UTIME_NOW) { t[k] = ts[2 * k]; explicit_time = true; }
        }
    }
    /* other times: the owner or root; "now" (touch): also whoever may write the file */
    if (cur->euid != 0) {
        long r = fs_call(FS_OP_STAT, ino, 0, 0);
        if (r < 0) return r;
        uint32_t fmode, fuid, fgid;
        memcpy(&fmode, (char *)fs_buf + 24, 4);
        memcpy(&fuid, (char *)fs_buf + 28, 4);
        memcpy(&fgid, (char *)fs_buf + 32, 4);
        if (fuid != cur->euid && (explicit_time || !cred_may(cur, fmode, fuid, fgid, 2)))
            return explicit_time ? -EPERM : -EACCES;
    }
    return fs_call(FS_OP_SETTIMES, ino, t[0], t[1]);
}

/* chown/lchown/fchownat: buf = path (empty + AT_EMPTY_PATH = dirfd itself) */
/* chown: root may give a file to anyone; its owner may only change its group, to one of its own */
static long may_chown(uint32_t fuid, long uid, long gid)
{
    if (cur->euid == 0) return 0;
    if (cur->euid != fuid) return -EPERM;
    if ((uint32_t)uid != (uint32_t)-1 && (uint32_t)uid != fuid) return -EPERM;
    if ((uint32_t)gid != (uint32_t)-1 && !cred_in_group(cur, gid)) return -EPERM;
    return 0;
}

static long do_chown(long dirfd, long uid, long gid, long flags)
{
    const char *path = client_path(0);
    if (!*path && (flags & AT_EMPTY_PATH)) {
        struct ofile *f = fd_get(dirfd);
        if (!f) return -EBADF;
        if (f->type != OF_FILE && f->type != OF_DIR) return 0;
        uint32_t fuid, fgid;
        long r = vfs_owner(f->ino, &fuid, &fgid);
        if (r || (r = may_chown(fuid, uid, gid))) return r;
        return fs_call(FS_OP_CHOWN, f->ino, uid, gid);
    }
    char abs[FS_PATH_MAX];
    long r = normalize(dirfd, path, abs);
    if (r) return r;
    if (dev_type(abs) != OF_FREE) return 0;
    long ino = fs_lookup(abs, !(flags & AT_SYMLINK_NOFOLLOW), NULL, NULL);
    if (ino < 0) return ino;
    if ((r = may_chown(last_uid, uid, gid))) return r;
    return fs_call(FS_OP_CHOWN, ino, uid, gid);
}

static long do_dup(long oldfd, long newfd, bool exact, bool cloexec)
{
    struct ofile *f = fd_get(oldfd);
    if (!f) return -EBADF;
    if (exact) {
        if (newfd < 0 || newfd >= NFD) return -EBADF;
        if (newfd == oldfd) return newfd;
        if (cur->fdt[newfd].of >= 0) fd_close(newfd);
    }
    int fd = fd_alloc(cur->fdt[oldfd].of, exact ? newfd : 0, cloexec);
    if (fd >= 0) f->refs++;
    return fd;
}

static long do_fcntl(long fd, long cmd, long arg)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int nfd = fd_alloc(cur->fdt[fd].of, arg, cmd == F_DUPFD_CLOEXEC);
        if (nfd >= 0) f->refs++;
        return nfd;
    }
    case F_GETFD: return cur->fdt[fd].cloexec ? FD_CLOEXEC : 0;
    case F_SETFD: cur->fdt[fd].cloexec = arg & FD_CLOEXEC; return 0;
    case F_GETFL: return f->flags;
    case F_SETFL: f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (arg & (O_APPEND | O_NONBLOCK)); return 0;
    case F_GETLK: case F_SETLK: case F_SETLKW: return 0;     /* a single process: no locks */
    case 1033: case 1034: return f->type == OF_SHM ? 0 : -EINVAL;   /* F_ADD_SEALS, F_GET_SEALS: no seals */
    }
    return -EINVAL;
}

bool vfs_handles(long nr)
{
    switch (nr) {
    case __NR_read: case __NR_write: case __NR_pread64: case __NR_pwrite64:
    case __NR_open: case __NR_openat: case __NR_close: case __NR_lseek:
    case __NR_stat: case __NR_lstat: case __NR_fstat: case __NR_newfstatat:
    case __NR_getdents64: case __NR_getcwd: case __NR_chdir: case __NR_fchdir:
    case __NR_mknod: case __NR_mknodat:
    case __NR_mkdir: case __NR_mkdirat: case __NR_rmdir: case __NR_unlink: case __NR_unlinkat:
    case __NR_rename: case __NR_renameat: case __NR_renameat2:
    case __NR_symlink: case __NR_symlinkat: case __NR_readlink: case __NR_readlinkat:
    case __NR_link: case __NR_linkat:
    case __NR_truncate: case __NR_ftruncate: case __NR_access: case __NR_faccessat: case __NR_faccessat2:
    case __NR_chmod: case __NR_fchmod: case __NR_fchmodat: case __NR_umask:
    case __NR_statfs: case __NR_fstatfs: case __NR_fcntl: case __NR_ioctl:
    case __NR_dup: case __NR_dup2: case __NR_dup3:
    case __NR_fsync: case __NR_fdatasync: case __NR_sync:
    case __NR_pipe2: case __NR_poll: case __NR_utimensat:
    case __NR_chown: case __NR_lchown: case __NR_fchown: case __NR_fchownat:
    case __NR_socket: case __NR_bind: case __NR_connect: case __NR_listen:
    case __NR_accept: case __NR_accept4: case __NR_sendto: case __NR_recvfrom:
    case __NR_shutdown: case __NR_setsockopt: case __NR_getsockopt:
    case __NR_getsockname: case __NR_getpeername: case __NR_socketpair:
    case __NR_eventfd: case __NR_eventfd2: case __NR_timerfd_create:
    case __NR_timerfd_settime: case __NR_timerfd_gettime:
    case __NR_epoll_create: case __NR_epoll_create1:
    case __NR_memfd_create: case __NR_mmap: case __NR_flock:
        return true;
    }
    return false;
}

long vfs_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    (void)a5;
    struct ofile *f;
    switch (nr) {
    case __NR_read:       return do_read(a1, a3, false, 0);
    case __NR_write:      return do_write(a1, a3, false, 0);
    case __NR_pread64:    return do_read(a1, a3, true, a4);
    case __NR_pwrite64:   return do_write(a1, a3, true, a4);
    case __NR_open:       return do_open(AT_FDCWD, a2, a3);
    case __NR_openat:     return do_open(a1, a3, a4);
    case __NR_close:      return fd_close(a1);
    case __NR_lseek:      return do_lseek(a1, a2, a3);
    case __NR_stat:       return do_stat(AT_FDCWD, 0);
    case __NR_lstat:      return do_stat(AT_FDCWD, AT_SYMLINK_NOFOLLOW);
    case __NR_fstat:      return do_fstat(a1);
    case __NR_newfstatat: return do_stat(a1, a4);
    case __NR_getdents64: return do_getdents(a1, a3);
    case __NR_getcwd: {
        size_t len = strlen(cur->cwd) + 1;
        if ((size_t)a2 < len) return -ERANGE;
        memcpy((void *)client_buf, cur->cwd, len);
        return len;
    }
    case __NR_chdir:      return path_op(__NR_chdir, AT_FDCWD, 0, 0);
    case __NR_fchdir:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type != OF_DIR && f->type != OF_PROCDIR) return -ENOTDIR;
        memcpy(cur->cwd, f->path, strlen(f->path) + 1);
        return 0;
    case __NR_mknod:      return path_op(__NR_mknod, AT_FDCWD, a2, 0);
    case __NR_mknodat:    return path_op(__NR_mknod, a1, a3, 0);
    case __NR_mkdir:      return path_op(__NR_mkdir, AT_FDCWD, a2, 0);
    case __NR_mkdirat:    return path_op(__NR_mkdir, a1, a3, 0);
    case __NR_rmdir:      return path_op(__NR_unlink, AT_FDCWD, 1, 0);
    case __NR_unlink:     return path_op(__NR_unlink, AT_FDCWD, 0, 0);
    case __NR_unlinkat:   return path_op(__NR_unlink, a1, (a3 & AT_REMOVEDIR) ? 1 : 0, 0);
    case __NR_rename:     return do_rename(AT_FDCWD, AT_FDCWD);
    case __NR_renameat:   return do_rename(a1, a3);
    case __NR_renameat2:  return a5 ? -EINVAL : do_rename(a1, a3);
    case __NR_link:       return do_link(AT_FDCWD, AT_FDCWD);
    case __NR_linkat:     return do_link(a1, a3);
    case __NR_symlink:    return do_symlink(AT_FDCWD);
    case __NR_symlinkat:  return do_symlink(a2);
    case __NR_readlink:   return path_op(__NR_readlink, AT_FDCWD, a3, 0);
    case __NR_readlinkat: return path_op(__NR_readlink, a1, a4, 0);
    case __NR_truncate:   return path_op(__NR_truncate, AT_FDCWD, a2, 0);
    case __NR_ftruncate:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type == OF_SHM) return a2 < 0 ? -EINVAL : mem_shm_truncate(f->ino, a2);
        if (f->type != OF_FILE || (f->flags & O_ACCMODE) == O_RDONLY) return -EINVAL;
        proc_image_release(f->ino);
        return fs_call(FS_OP_TRUNCATE, f->ino, a2, 0);
    case __NR_access:     return path_op(__NR_access, AT_FDCWD, a2, 0);
    case __NR_faccessat:
    case __NR_faccessat2: return path_op(__NR_access, a1, a3, 0);
    case __NR_chmod:      return path_op(__NR_chmod, AT_FDCWD, a2, 0);
    case __NR_fchmodat:   return path_op(__NR_chmod, a1, a3, 0);
    case __NR_fchmod:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type != OF_FILE && f->type != OF_DIR) return -EINVAL;
        {
            uint32_t fuid, fgid;
            long r = vfs_owner(f->ino, &fuid, &fgid);
            if (r || (r = may_own(fuid))) return r;
        }
        return fs_call(FS_OP_CHMOD, f->ino, a2, 0);
    case __NR_umask: {
        long old = cur->umask;
        cur->umask = a1 & 0777;
        return old;
    }
    case __NR_statfs:     return path_op(__NR_statfs, AT_FDCWD, 0, 0);
    case __NR_fstatfs: {
        if (!fd_get(a1)) return -EBADF;
        long r = fs_call(FS_OP_STATFS, 0, 0, 0);
        if (r >= 0) memcpy((void *)client_buf, (void *)fs_buf, K_STATFS_SIZE);
        return r;
    }
    case __NR_fcntl:      return do_fcntl(a1, a2, a3);
    case __NR_ioctl:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (a2 == FIONBIO) {                               /* arg: the integer, by value */
            f->flags = a3 ? f->flags | O_NONBLOCK : f->flags & ~O_NONBLOCK;
            return 0;
        }
        if (f->type == OF_SOCKET && a2 == NET_SIOC_CONFIG) {    /* network configuration (apps/netcfg) */
            memcpy((void *)net_buf, (void *)client_buf, sizeof(struct net_cfg));
            long r = net_call(NET_OP_CONFIG, 0, 0, 0, 0);
            if (r == 0) net_lease_changed();                    /* DNS of the static address */
            return r;
        }
        if (f->type == OF_SOCKET && a2 == FIONREAD) {
            long n = net_call(NET_OP_FIONREAD, f->ino, 0, 0, 0);
            if (n >= 0) microkit_mr_set(1, n);
            return n < 0 ? n : 0;
        }
        return f->type == OF_TTY ? tty_ioctl(a2, a3) : -ENOTTY;
    case __NR_dup:        return do_dup(a1, 0, false, false);
    case __NR_dup2:       return do_dup(a1, a2, true, false);
    case __NR_dup3:       return a1 == a2 ? -EINVAL : do_dup(a1, a2, true, a3 & O_CLOEXEC);
    case __NR_fsync:
    case __NR_fdatasync:  return fd_get(a1) ? 0 : -EBADF;   /* fs_ext2 writes immediately */
    case __NR_sync:       fs_call(FS_OP_SYNC, 0, 0, 0); return 0;
    case __NR_pipe2:      return do_pipe(a1);
    case __NR_poll:       return do_poll(a1, a2);
    case __NR_utimensat:  return do_utimensat(a1, a3, a4);
    case __NR_chown:      return do_chown(AT_FDCWD, a2, a3, 0);
    case __NR_lchown:     return do_chown(AT_FDCWD, a2, a3, AT_SYMLINK_NOFOLLOW);
    case __NR_fchownat:   return do_chown(a1, a3, a4, a5);
    case __NR_fchown:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type != OF_FILE && f->type != OF_DIR) return 0;   /* tty, pipes: no owner on disk */
        {
            uint32_t fuid, fgid;
            long r = vfs_owner(f->ino, &fuid, &fgid);
            if (r || (r = may_chown(fuid, a2, a3))) return r;
        }
        return fs_call(FS_OP_CHOWN, f->ino, a2, a3);
    }
    switch (nr) {
    case __NR_memfd_create: {                          /* a2: MFD_CLOEXEC (1), MFD_ALLOW_SEALING */
        long id = mem_shm_new();
        if (id < 0) return id;
        int of = ofile_alloc(OF_SHM);
        if (of < 0) { mem_shm_unref(id); return -ENFILE; }
        ofiles[of].ino = id;
        ofiles[of].flags = 2;                          /* O_RDWR */
        int fd = fd_alloc(of, 0, a2 & 1);
        if (fd < 0) ofile_release(of);
        return fd;
    }
    case __NR_mmap:                                    /* shared memfd only; the runtime does the rest */
        f = fd_get(a5);
        if (!f) return -EBADF;
        if (f->type != OF_SHM || !(a4 & 1)) return -ENODEV;   /* MAP_SHARED */
        return mem_shm_map(cur->slot, f->ino, a1, a2, a6, a3 & 2);   /* PROT_WRITE */
    case __NR_flock:                                   /* file locks: a single user, accepted */
        return fd_get(a1) ? 0 : -EBADF;
    case __NR_eventfd:         return evfd_new(a1, 0);
    case __NR_eventfd2:        return evfd_new(a1, a2);
    case __NR_timerfd_create:  return tfd_new(a2);
    case __NR_epoll_create:
    case __NR_epoll_create1: {
        int of = ofile_alloc(OF_EPOLL);
        if (of < 0) return -ENFILE;
        int fd = fd_alloc(of, 0, nr == __NR_epoll_create1 && (a1 & O_CLOEXEC));
        if (fd < 0) ofile_release(of);
        return fd;
    }
    case __NR_timerfd_settime:
    case __NR_timerfd_gettime:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type != OF_TIMERFD) return -EINVAL;
        return tfd_settime(f, nr == __NR_timerfd_settime, a2, a3);
    }
    if (nr == __NR_socket) return sock_socket(a1, a2, a3);
    if (nr == __NR_socketpair) return a1 == AF_UNIX_ ? usock_pair(a2) : -EOPNOTSUPP;
    f = fd_get(a1);
    if (!f) return -EBADF;
    if (f->type == OF_USOCK) return usock_syscall(nr, f, a2, a3, a4, a5, a6);
    if (f->type != OF_SOCKET) return -ENOTSOCK;
    return sock_syscall(nr, f, a2, a3, a4, a5, a6);
}

void vfs_init(void)
{
}

/* Hands the clock to fs_ext2 to timestamp files */
/* Shutdown: the file system writes everything, marks itself clean and refuses further writes */
void vfs_unmount(void)
{
    fs_call(FS_OP_SYNC, 1, 0, 0);
}

void vfs_set_fs_clock(void)
{
    fs_call(FS_OP_SETCLOCK, clock_epoch, clock_tsc0, clock_tsc_hz);
}

/* 0, 1 and 2 to the terminal (initial process) */
void vfs_proc_tty(struct proc *p)
{
    for (int i = 0; i < NFD; i++) p->fdt[i].of = -1;
    int tty = ofile_alloc(OF_TTY);
    ofiles[tty].flags = 2;  /* O_RDWR */
    memcpy(ofiles[tty].path, "/dev/tty", 9);
    ofiles[tty].refs = 3;
    for (int fd = 0; fd < 3; fd++) p->fdt[fd].of = tty;
    memcpy(p->cwd, "/", 2);
    p->umask = 022;
}

/* fork: the child inherits the descriptors (they share the open file description) */
void vfs_fork(struct proc *child, struct proc *parent)
{
    for (int i = 0; i < NFD; i++) {
        child->fdt[i] = parent->fdt[i];
        if (child->fdt[i].of >= 0) ofiles[child->fdt[i].of].refs++;
    }
    memcpy(child->cwd, parent->cwd, strlen(parent->cwd) + 1);
    child->umask = parent->umask;
}

void vfs_close_all(struct proc *p)
{
    fifo_cancel_wait(p);
    for (int i = 0; i < NFD; i++) {
        if (p->fdt[i].of >= 0) {
            ofile_release(p->fdt[i].of);
            p->fdt[i].of = -1;
        }
    }
}

void vfs_close_cloexec(struct proc *p)
{
    for (int i = 0; i < NFD; i++) {
        if (p->fdt[i].of >= 0 && p->fdt[i].cloexec) {
            ofile_release(p->fdt[i].of);
            p->fdt[i].of = -1;
        }
    }
}

long vfs_normalize(struct proc *p, const char *path, char *out)
{
    struct proc *saved = cur;
    cur = p;
    long r = normalize(AT_FDCWD, path, out);
    cur = saved;
    return r;
}

long vfs_lookup(const char *abs, bool follow, uint32_t *mode, uint64_t *size)
{
    return fs_lookup(abs, follow, mode, size);
}

/* Owner of an inode (struct stat: st_uid at 28, st_gid at 32) */
long vfs_owner(uint32_t ino, uint32_t *uid, uint32_t *gid)
{
    long r = fs_call(FS_OP_STAT, ino, 0, 0);
    if (r < 0) return r;
    memcpy(uid, (char *)fs_buf + 28, 4);
    memcpy(gid, (char *)fs_buf + 32, 4);
    return 0;
}

/* Reads from a file into server memory (for the program loader) */
/* Reads a file straight into a process's memory (exec), from fs_ext2's buffer */
long vfs_read_to_proc(uint32_t ino, uint64_t off, int slot, uintptr_t vaddr, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done < FS_BUF_SIZE ? len - done : FS_BUF_SIZE;
        long r = fs_call(FS_OP_READ, ino, off + done, chunk);
        if (r < 0) return r;
        if (r == 0) break;
        if (!mem_copy_in(slot, vaddr + done, (void *)fs_buf, r)) return -ENOMEM;
        done += r;
    }
    return done;
}

long vfs_read_file(uint32_t ino, uint64_t off, void *dst, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done < FS_BUF_SIZE ? len - done : FS_BUF_SIZE;
        long r = fs_call(FS_OP_READ, ino, off + done, chunk);
        if (r < 0) return r;
        if (r == 0) break;
        memcpy((char *)dst + done, (void *)fs_buf, r);
        done += r;
    }
    return done;
}
