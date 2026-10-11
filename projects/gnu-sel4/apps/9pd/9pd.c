/*
 * 9pd: shares the GNU/seL4 file system with the Linux VM over 9P2000.L
 * (what WSL2 does with /mnt/c).
 *
 *   9pd [mount-point-in-the-VM]      (default /mnt/sel4)
 *
 * It opens a bridge session (/dev/linux) asking the VM agent for the internal
 * command "@9p-mount <point>": the agent mounts 9p with trans=fd and forwards
 * the bytes of the Linux kernel's 9P client over the session. Here each message
 * is served with plain POSIX calls (open, pread, getdents...), so fs_ext2 stays
 * the only owner of the disk.
 *
 * Permissions: the VM mounts with access=user, so each VM user attaches with its own uid, and
 * every request is served with that user's native credentials (9pd runs as root and switches its
 * effective IDs). The VM's root is squashed to nobody (as NFS does): being root inside the VM
 * (Docker) gives no power over native files.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <grp.h>
#include <pwd.h>
#include <sel4posix/bridge.h>

#define MSIZE 131072                 /* 9P message size: fewer round trips over the bridge */

/* ---------------- 9P2000.L messages ---------------- */

enum {
    Tlerror = 6, Rlerror, Tstatfs = 8, Rstatfs, Tlopen = 12, Rlopen, Tlcreate = 14, Rlcreate,
    Tsymlink = 16, Rsymlink, Tmknod = 18, Rmknod, Trename = 20, Rrename, Treadlink = 22, Rreadlink,
    Tgetattr = 24, Rgetattr, Tsetattr = 26, Rsetattr, Txattrwalk = 30, Rxattrwalk,
    Txattrcreate = 32, Rxattrcreate, Treaddir = 40, Rreaddir, Tfsync = 50, Rfsync,
    Tlock = 52, Rlock, Tgetlock = 54, Rgetlock, Tlink = 70, Rlink, Tmkdir = 72, Rmkdir,
    Trenameat = 74, Rrenameat, Tunlinkat = 76, Runlinkat,
    Tversion = 100, Rversion, Tauth = 102, Rauth, Tattach = 104, Rattach,
    Tflush = 108, Rflush, Twalk = 110, Rwalk, Tread = 116, Rread, Twrite = 118, Rwrite,
    Tclunk = 120, Rclunk, Tremove = 122, Rremove,
};

/* Reading a received message */
struct in { const uint8_t *p, *end; int bad; };
static uint8_t  g8(struct in *m)  { if (m->p + 1 > m->end) { m->bad = 1; return 0; } return *m->p++; }
static uint16_t g16(struct in *m) { uint16_t v = g8(m); return v | g8(m) << 8; }
static uint32_t g32(struct in *m) { uint32_t v = g16(m); return v | (uint32_t)g16(m) << 16; }
static uint64_t g64(struct in *m) { uint64_t v = g32(m); return v | (uint64_t)g32(m) << 32; }
/* 9P string -> \0-terminated copy */
static char *gstr(struct in *m, char *buf, size_t max)
{
    uint16_t n = g16(m);
    if (m->p + n > m->end || n >= max) { m->bad = 1; buf[0] = 0; return buf; }
    memcpy(buf, m->p, n);
    buf[n] = 0;
    m->p += n;
    return buf;
}

/* Building the reply */
static uint8_t out[MSIZE];
static size_t olen;
static void p8(uint8_t v)   { if (olen < MSIZE) out[olen++] = v; }
static void p16(uint16_t v) { p8(v); p8(v >> 8); }
static void p32(uint32_t v) { p16(v); p16(v >> 16); }
static void p64(uint64_t v) { p32(v); p32(v >> 32); }
static void pstr(const char *s) { size_t n = strlen(s); p16(n); for (size_t i = 0; i < n; i++) p8(s[i]); }
static void begin(int type, uint16_t tag) { olen = 4; p8(type); p16(tag); }

static void pqid(const struct stat *st)
{
    p8(S_ISDIR(st->st_mode) ? 0x80 : S_ISLNK(st->st_mode) ? 0x02 : 0);
    p32(st->st_mtime);
    p64(st->st_ino);
}

/* ---------------- fids ---------------- */

#define NFID 256
static struct fid { int used; uint32_t id; char path[1024]; int fd; uint32_t uid; } fids[NFID];

/* ---------------- credentials ---------------- */

#define NOBODY 65534
static uint32_t as_uid;                 /* whose effective IDs 9pd has now (0 = its own, root) */

/* Takes the native credentials of uid: its gid and supplementary groups from /etc/passwd and
 * /etc/group (a uid with no account gets nobody's group and none) */
static void become(uint32_t uid)
{
    if (uid == as_uid) return;
    if (as_uid != 0) {
        if (seteuid(0)) return;
        as_uid = 0;
    }
    gid_t gid = uid == 0 ? 0 : NOBODY, groups[32];
    int ng = 0;
    struct passwd *pw = uid == 0 ? NULL : getpwuid(uid);
    if (pw) {
        gid = pw->pw_gid;
        ng = 32;
        if (getgrouplist(pw->pw_name, gid, groups, &ng) < 0) ng = 0;
    }
    if (setgroups(ng, groups) || setegid(gid)) return;
    if (uid == 0 || seteuid(uid) == 0) as_uid = uid;
}

static struct fid *fid_get(uint32_t id)
{
    for (int i = 0; i < NFID; i++) if (fids[i].used && fids[i].id == id) return &fids[i];
    return NULL;
}

static struct fid *fid_new(uint32_t id, const char *path, uint32_t uid)
{
    struct fid *f = fid_get(id);
    if (!f) for (int i = 0; i < NFID && !f; i++) if (!fids[i].used) f = &fids[i];
    if (!f) return NULL;
    if (f->used && f->fd >= 0) close(f->fd);
    f->used = 1;
    f->id = id;
    f->fd = -1;
    f->uid = uid;
    snprintf(f->path, sizeof f->path, "%s", path);
    return f;
}

static void fid_free(struct fid *f)
{
    if (f->fd >= 0) close(f->fd);
    f->used = 0;
}

static void join(char *dst, size_t max, const char *dir, const char *name)
{
    if (!strcmp(name, "..")) {                      /* go up, never past the root */
        snprintf(dst, max, "%s", dir);
        char *s = strrchr(dst, '/');
        if (s && s != dst) *s = 0; else strcpy(dst, "/");
    } else if (!strcmp(name, ".")) {
        snprintf(dst, max, "%s", dir);
    } else {
        snprintf(dst, max, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
    }
}

/* ---------------- serving messages ---------------- */

static int handle(const uint8_t *msg, size_t len)
{
    struct in m = { msg + 4, msg + len, 0 };
    int type = g8(&m);
    uint16_t tag = g16(&m);
    struct fid *f;
    struct stat st;
    char name[256], path[1024], tgt[1024];
    int err = 0;

#define NEED_FID(id) do { f = fid_get(id); if (!f) { err = EBADF; goto fail; } become(f->uid); } while (0)

    switch (type) {
    case Tversion: {
        uint32_t msize = g32(&m);
        char ver[32];
        gstr(&m, ver, sizeof ver);
        for (int i = 0; i < NFID; i++) if (fids[i].used) fid_free(&fids[i]);
        begin(Rversion, tag);
        p32(msize < MSIZE ? msize : MSIZE);
        pstr(strcmp(ver, "9P2000.L") ? "unknown" : "9P2000.L");
        break;
    }
    case Tattach: {
        uint32_t fid = g32(&m);
        g32(&m);                                   /* afid: no authentication */
        gstr(&m, name, sizeof name);               /* uname, aname */
        gstr(&m, name, sizeof name);
        uint32_t uid = g32(&m);                    /* n_uname: the VM user's uid */
        if (uid == 0 || uid == 0xffffffffu) uid = NOBODY;   /* root squashing */
        become(uid);
        if (!fid_new(fid, "/", uid) || lstat("/", &st)) { err = ENOMEM; goto fail; }
        begin(Rattach, tag);
        pqid(&st);
        break;
    }
    case Twalk: {
        uint32_t fid = g32(&m), newfid = g32(&m);
        uint16_t n = g16(&m);
        NEED_FID(fid);
        char cur[1024];
        snprintf(cur, sizeof cur, "%s", f->path);
        size_t qpos;
        begin(Rwalk, tag);
        qpos = olen;
        p16(0);
        int walked = 0;
        for (; walked < n; walked++) {
            gstr(&m, name, sizeof name);
            join(path, sizeof path, cur, name);
            if (lstat(path, &st)) {
                if (!walked) { err = errno; goto fail; }
                break;
            }
            pqid(&st);
            snprintf(cur, sizeof cur, "%s", path);
        }
        out[qpos] = walked;
        out[qpos + 1] = walked >> 8;
        if (walked == n && !fid_new(newfid, cur, f->uid)) { err = ENOMEM; goto fail; }
        break;
    }
    case Tgetattr: {
        NEED_FID(g32(&m));
        if (lstat(f->path, &st)) { err = errno; goto fail; }
        begin(Rgetattr, tag);
        p64(0x3fff);                               /* P9_GETATTR_BASIC | BTIME | GEN | DATA_VERSION */
        pqid(&st);
        p32(st.st_mode); p32(st.st_uid); p32(st.st_gid);
        p64(st.st_nlink); p64(st.st_rdev); p64(st.st_size); p64(st.st_blksize); p64(st.st_blocks);
        p64(st.st_atime); p64(0); p64(st.st_mtime); p64(0); p64(st.st_ctime); p64(0);
        p64(0); p64(0); p64(0); p64(0);            /* btime, gen, data_version */
        break;
    }
    case Tsetattr: {
        NEED_FID(g32(&m));
        uint32_t valid = g32(&m), mode = g32(&m);
        g32(&m); g32(&m);                          /* uid, gid: no owners for now */
        uint64_t size = g64(&m);
        if ((valid & 1) && chmod(f->path, mode & 07777)) { err = errno; goto fail; }
        if ((valid & 8) && truncate(f->path, size)) { err = errno; goto fail; }
        begin(Rsetattr, tag);
        break;
    }
    case Tlopen: {
        NEED_FID(g32(&m));
        uint32_t flags = g32(&m) & ~(O_CREAT | O_EXCL | O_NOCTTY);
        if (lstat(f->path, &st)) { err = errno; goto fail; }
        if (f->fd >= 0) close(f->fd);
        f->fd = S_ISDIR(st.st_mode) ? open(f->path, O_RDONLY | O_DIRECTORY) : open(f->path, flags);
        if (f->fd < 0) { err = errno; goto fail; }
        begin(Rlopen, tag);
        pqid(&st);
        p32(0);
        break;
    }
    case Tlcreate: {
        NEED_FID(g32(&m));
        gstr(&m, name, sizeof name);
        uint32_t flags = g32(&m), mode = g32(&m);
        join(path, sizeof path, f->path, name);
        int fd = open(path, flags | O_CREAT, mode & 07777);
        if (fd < 0 || lstat(path, &st)) { err = errno; if (fd >= 0) close(fd); goto fail; }
        if (f->fd >= 0) close(f->fd);
        f->fd = fd;
        snprintf(f->path, sizeof f->path, "%s", path);
        begin(Rlcreate, tag);
        pqid(&st);
        p32(0);
        break;
    }
    case Tread: {
        NEED_FID(g32(&m));
        uint64_t off = g64(&m);
        uint32_t count = g32(&m);
        if (count > MSIZE - 11) count = MSIZE - 11;
        if (f->fd < 0) { err = EBADF; goto fail; }
        begin(Rread, tag);
        ssize_t n = pread(f->fd, out + olen + 4, count, off);
        if (n < 0) { err = errno; goto fail; }
        p32(n);
        olen += n;
        break;
    }
    case Twrite: {
        NEED_FID(g32(&m));
        uint64_t off = g64(&m);
        uint32_t count = g32(&m);
        if (m.p + count > m.end || f->fd < 0) { err = EBADF; goto fail; }
        ssize_t n = pwrite(f->fd, m.p, count, off);
        if (n < 0) { err = errno; goto fail; }
        begin(Rwrite, tag);
        p32(n);
        break;
    }
    case Treaddir: {
        NEED_FID(g32(&m));
        uint64_t off = g64(&m);
        uint32_t count = g32(&m);
        if (count > MSIZE - 11) count = MSIZE - 11;
        DIR *d = opendir(f->path);
        if (!d) { err = errno; goto fail; }
        begin(Rreaddir, tag);
        size_t cpos = olen;
        p32(0);
        uint64_t idx = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (idx++ < off) continue;              /* the offset is the entry number */
            size_t need = 13 + 8 + 1 + 2 + strlen(e->d_name);
            if (olen - cpos - 4 + need > count) break;
            join(path, sizeof path, f->path, e->d_name);
            if (lstat(path, &st)) memset(&st, 0, sizeof st);
            pqid(&st);
            p64(idx);
            p8(e->d_type);
            pstr(e->d_name);
        }
        closedir(d);
        uint32_t used = olen - cpos - 4;
        out[cpos] = used; out[cpos + 1] = used >> 8; out[cpos + 2] = used >> 16; out[cpos + 3] = used >> 24;
        break;
    }
    case Tmkdir: {
        NEED_FID(g32(&m));
        gstr(&m, name, sizeof name);
        uint32_t mode = g32(&m);
        join(path, sizeof path, f->path, name);
        if (mkdir(path, mode & 07777) || lstat(path, &st)) { err = errno; goto fail; }
        begin(Rmkdir, tag);
        pqid(&st);
        break;
    }
    case Tsymlink: {
        NEED_FID(g32(&m));
        gstr(&m, name, sizeof name);
        gstr(&m, tgt, sizeof tgt);
        join(path, sizeof path, f->path, name);
        if (symlink(tgt, path) || lstat(path, &st)) { err = errno; goto fail; }
        begin(Rsymlink, tag);
        pqid(&st);
        break;
    }
    case Treadlink: {
        NEED_FID(g32(&m));
        ssize_t n = readlink(f->path, tgt, sizeof tgt - 1);
        if (n < 0) { err = errno; goto fail; }
        tgt[n] = 0;
        begin(Rreadlink, tag);
        pstr(tgt);
        break;
    }
    case Trenameat: {
        struct fid *od = fid_get(g32(&m));
        gstr(&m, name, sizeof name);
        struct fid *nd = fid_get(g32(&m));
        char nname[256];
        gstr(&m, nname, sizeof nname);
        if (!od || !nd) { err = EBADF; goto fail; }
        join(path, sizeof path, od->path, name);
        join(tgt, sizeof tgt, nd->path, nname);
        if (rename(path, tgt)) { err = errno; goto fail; }
        begin(Rrenameat, tag);
        break;
    }
    case Trename: {
        NEED_FID(g32(&m));
        struct fid *nd = fid_get(g32(&m));
        gstr(&m, name, sizeof name);
        if (!nd) { err = EBADF; goto fail; }
        join(tgt, sizeof tgt, nd->path, name);
        if (rename(f->path, tgt)) { err = errno; goto fail; }
        snprintf(f->path, sizeof f->path, "%s", tgt);
        begin(Rrename, tag);
        break;
    }
    case Tunlinkat: {
        NEED_FID(g32(&m));
        gstr(&m, name, sizeof name);
        uint32_t flags = g32(&m);
        join(path, sizeof path, f->path, name);
        if ((flags & 0x200) ? rmdir(path) : unlink(path)) { err = errno; goto fail; }
        begin(Runlinkat, tag);
        break;
    }
    case Tremove: {
        NEED_FID(g32(&m));
        int r = lstat(f->path, &st) ? -1 : S_ISDIR(st.st_mode) ? rmdir(f->path) : unlink(f->path);
        int e = errno;
        fid_free(f);
        if (r) { err = e; goto fail; }
        begin(Rremove, tag);
        break;
    }
    case Tclunk: {
        NEED_FID(g32(&m));
        fid_free(f);
        begin(Rclunk, tag);
        break;
    }
    case Tstatfs: {
        NEED_FID(g32(&m));
        struct statvfs sv;
        if (statvfs(f->path, &sv)) { err = errno; goto fail; }
        begin(Rstatfs, tag);
        p32(0xEF53); p32(sv.f_bsize); p64(sv.f_blocks); p64(sv.f_bfree); p64(sv.f_bavail);
        p64(sv.f_files); p64(sv.f_ffree); p64(0); p32(255);
        break;
    }
    case Tfsync:  begin(Rfsync, tag); break;
    case Tflush:  begin(Rflush, tag); break;
    case Tlock:   begin(Rlock, tag); p8(0); break;          /* a single client: always granted */
    case Tgetlock: {
        g32(&m);
        g8(&m);                                             /* requested type */
        uint64_t start = g64(&m), length = g64(&m);
        uint32_t pid = g32(&m);
        gstr(&m, name, sizeof name);
        begin(Rgetlock, tag);
        p8(2); p64(start); p64(length); p32(pid); pstr(name);   /* F_UNLCK: free */
        break;
    }
    case Txattrwalk:                                        /* no extended attributes */
    case Txattrcreate:
        err = EOPNOTSUPP;
        goto fail;
    case Tlink:                                             /* hard links over 9P: not yet (fs_ext2 has them) */
    case Tmknod:
    case Tauth:
        err = EOPNOTSUPP;
        goto fail;
    default:
        err = EOPNOTSUPP;
        goto fail;
    }
    if (m.bad) { err = EINVAL; goto fail; }
    goto done;
fail:
    begin(Rlerror, tag);
    p32(err ? err : EIO);
done:
    out[0] = olen; out[1] = olen >> 8; out[2] = olen >> 16; out[3] = olen >> 24;
    return 0;
}

/* ---------------- session with the VM ---------------- */

static int sfd;

static void send_bytes(const uint8_t *p, size_t len)
{
    static uint8_t rec[1 + BR_MAX_DATA];
    while (len) {
        size_t n = len < BR_MAX_DATA ? len : BR_MAX_DATA;
        rec[0] = BR_STDIN;
        memcpy(rec + 1, p, n);
        if (write(sfd, rec, n + 1) < 0) { perror("9pd: escribir"); exit(1); }
        p += n;
        len -= n;
    }
}

int main(int argc, char **argv)
{
    const char *mnt = argc > 1 ? argv[1] : "/mnt/sel4";
    sfd = open("/dev/linux", O_RDWR);
    if (sfd < 0) { perror("9pd: /dev/linux"); return 1; }

    /* Ask the agent to mount 9p and hand us the bytes of that connection */
    static char req[512];
    struct br_open op = { .flags = 0, .argc = 2 };
    size_t len = sizeof op;
    len += sprintf(req + len, "@9p-mount") + 1;
    len += sprintf(req + len, "%s", mnt) + 1;
    memcpy(req, &op, sizeof op);
    static uint8_t rec[1 + BR_MAX_DATA + 16];
    rec[0] = BR_OPEN;
    memcpy(rec + 1, req, len);
    if (write(sfd, rec, len + 1) < 0) { perror("9pd: opening a session"); return 1; }

    static uint8_t inbuf[2 * MSIZE + BR_MAX_DATA];
    size_t have = 0;
    for (;;) {
        ssize_t n = read(sfd, rec, sizeof rec);
        if (n <= 0) { fprintf(stderr, "9pd: lost the connection to the VM\n"); return 1; }
        if (rec[0] == BR_EXIT) {
            int32_t code = 0;
            memcpy(&code, rec + 1, 4);
            fprintf(stderr, "9pd: the mount ended (code %d)\n", code);
            return code ? code : 1;
        }
        if (rec[0] == BR_STDERR) { fwrite(rec + 1, 1, n - 1, stderr); continue; }
        if (rec[0] != BR_STDOUT) continue;
        if (have + n - 1 > sizeof inbuf) have = 0;           /* corrupt stream: restart */
        memcpy(inbuf + have, rec + 1, n - 1);
        have += n - 1;
        size_t off = 0;
        while (have - off >= 7) {
            uint32_t size = inbuf[off] | inbuf[off + 1] << 8 | inbuf[off + 2] << 16 | (uint32_t)inbuf[off + 3] << 24;
            if (size < 7 || size > MSIZE) { off = have; break; }
            if (have - off < size) break;
            handle(inbuf + off, size);
            send_bytes(out, olen);
            off += size;
        }
        memmove(inbuf, inbuf + off, have - off);
        have -= off;
    }
}
