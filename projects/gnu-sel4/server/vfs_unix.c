/*
 * posix_server VFS: local sockets (AF_UNIX) (see vfs.h).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vfs.h"

/* ---------------- local sockets (AF_UNIX, SOCK_STREAM and SOCK_DGRAM) ---------------- */

/* They live in the server: each connected socket has its input buffer (what its peer
 * writes to it) and a queue of descriptors in transit (SCM_RIGHTS: references to the
 * open file description are held until the other process receives them).
 * Names (bind) are in a server table, not on disk: unlink and stat of the path see
 * them. sendmsg/recvmsg data: the descriptors go at NET_ADDR_OFF of the process's
 * buffer. */
#define NUSOCK        128
#define USOCK_BUF     32768
#define USOCK_MAXFD   32
#define SO_PEERCRED   17
#define MSG_CMSG_CLOEXEC 0x40000000
#define EADDRINUSE    98
#define ECONNREFUSED  111
#define EPROTONOSUPPORT 93
#define ENOTCONN      107
enum { US_FREE, US_NEW, US_LISTEN, US_CONN };

static struct usock {
    int state;
    bool dgram;                            /* SOCK_DGRAM: each send is one message [len 4][data] */
    int peer;                              /* index of the peer (US_CONN) */
    bool peer_closed;
    int pid;                               /* process that created it (the peer's SO_PEERCRED) */
    char name[108];                        /* bind: absolute path or "@abstract" */
    uint32_t ino;                          /* bound to a path: the socket's inode on disk */
    uint32_t head, tail;                   /* input buffer */
    uint8_t buf[USOCK_BUF];
    int fdq[USOCK_MAXFD], nfdq;            /* open file descriptions in transit (ofiles indices) */
    int pend[8], npend;                    /* US_LISTEN: connections waiting to be accepted */
    waitmask_t rwait, wwait;
} usocks[NUSOCK];

static int usock_alloc(void)
{
    for (int i = 0; i < NUSOCK; i++)
        if (usocks[i].state == US_FREE) {
            memset(&usocks[i], 0, offsetof(struct usock, buf));
            usocks[i].state = US_NEW;
            usocks[i].peer = -1;
            usocks[i].pid = cur->pid;
            return i;
        }
    return -1;
}

void usock_wake(struct usock *u)
{
    proc_wake_mask(u->rwait | u->wwait);
    u->rwait = u->wwait = 0;
}

void usock_free(int i)
{
    struct usock *u = &usocks[i];
    for (int k = 0; k < u->nfdq; k++) ofile_release(u->fdq[k]);
    for (int k = 0; k < u->npend; k++) usock_free(u->pend[k]);   /* connections never accepted */
    if (u->state == US_CONN && u->peer >= 0) {
        usocks[u->peer].peer_closed = true;
        usock_wake(&usocks[u->peer]);
    }
    u->state = US_FREE;
}

/* sockaddr_un at NET_ADDR_OFF of the process's buffer -> name */
static long usock_name(long alen, char *out)
{
    const char *sa = (const char *)(client_buf + NET_ADDR_OFF);
    if (alen <= 2 || alen > 110) return -EINVAL;
    if (sa[2] == 0) {                                   /* Linux abstract namespace */
        size_t n = alen - 3 < 106 ? alen - 3 : 106;
        out[0] = '@';
        memcpy(out + 1, sa + 3, n);
        out[n + 1] = 0;
        return 0;
    }
    char path[109], abs[FS_PATH_MAX];
    size_t n = alen - 2;
    memcpy(path, sa + 2, n);
    path[n] = 0;
    long r = normalize(AT_FDCWD, path, abs);
    if (r) return r;
    if (strlen(abs) >= 108) return -ENAMETOOLONG;
    memcpy(out, abs, strlen(abs) + 1);
    return 0;
}

/* The listening socket a name refers to, or -errno. A path is resolved through the file system
 * (symlinks included) to its socket inode, as in Unix: the same socket can be reached by any
 * path to it. The socket file stays after close until unlink (connect -> ECONNREFUSED). An
 * abstract name (Linux's "@...") exists only while its socket does. */
static long usock_lookup(const char *name)
{
    uint32_t mode = 0;
    long ino = 0;
    if (name[0] != '@') {
        ino = fs_lookup(name, true, &mode, NULL);
        if (ino < 0) return ino;
        if ((mode & S_IFMT) != S_IFSOCK_) return -ECONNREFUSED;
        if (!cred_may(cur, mode, last_uid, last_gid, 2)) return -EACCES;   /* connect needs write */
    }
    for (int i = 0; i < NUSOCK; i++) {
        if (usocks[i].state != US_LISTEN) continue;
        if (name[0] == '@' ? !strcmp(usocks[i].name, name) : usocks[i].ino == (uint32_t)ino) return i;
    }
    return -ECONNREFUSED;
}

static long usock_fd(int ui, long flags)
{
    int of = ofile_alloc(OF_USOCK);
    if (of < 0) return -ENFILE;
    ofiles[of].ino = ui;
    ofiles[of].flags = 2 | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0);
    int fd = fd_alloc(of, 0, flags & SOCK_CLOEXEC);
    if (fd < 0) ofile_release(of);
    return fd;
}

long usock_new(long type)
{
    if ((type & 0xf) != 1 && (type & 0xf) != 2) return -EPROTONOSUPPORT;   /* STREAM and DGRAM */
    int u = usock_alloc();
    if (u < 0) return -ENFILE;
    usocks[u].dgram = (type & 0xf) == 2;
    long fd = usock_fd(u, type);
    if (fd < 0) usocks[u].state = US_FREE;
    return fd;
}

long usock_pair(long type)
{
    if ((type & 0xf) != 1 && (type & 0xf) != 2) return -EPROTONOSUPPORT;
    int a = usock_alloc(), b = a >= 0 ? usock_alloc() : -1;
    if (b < 0) {
        if (a >= 0) usocks[a].state = US_FREE;
        return -ENFILE;
    }
    usocks[a].dgram = usocks[b].dgram = (type & 0xf) == 2;
    usocks[a].state = usocks[b].state = US_CONN;
    usocks[a].peer = b;
    usocks[b].peer = a;
    long fa = usock_fd(a, type);
    if (fa < 0) { usocks[a].state = usocks[b].state = US_FREE; return fa; }
    long fb = usock_fd(b, type);
    if (fb < 0) { fd_close(fa); return fb; }
    microkit_mr_set(1, fa);
    microkit_mr_set(2, fb);
    return 0;
}

long usock_bind(struct ofile *f, long alen)
{
    struct usock *u = &usocks[f->ino];
    char name[108];
    long r = usock_name(alen, name);
    if (r) return r;
    if (u->name[0]) return -EINVAL;
    if (name[0] == '@') {                               /* abstract: unique among live sockets */
        for (int i = 0; i < NUSOCK; i++)
            if (usocks[i].state != US_FREE && !strcmp(usocks[i].name, name)) return -EADDRINUSE;
    } else {                                            /* a socket file: fails if the path exists */
        if ((r = may_change_dir(name))) return r;
        fs_set_path(0, name);
        long ino = fs_call(FS_OP_CREATE, S_IFSOCK_ | (0777 & ~cur->umask), cur->euid, cur->egid);
        if (ino == -EEXIST) return -EADDRINUSE;
        if (ino < 0) return ino;
        u->ino = ino;
    }
    memcpy(u->name, name, sizeof name);
    return 0;
}

long usock_connect(struct ofile *f, long alen)
{
    struct usock *u = &usocks[f->ino];
    if (u->state == US_CONN) return -EISCONN;
    if (u->state != US_NEW) return -EINVAL;
    char name[108];
    long r = usock_name(alen, name);
    if (r) return r;
    long li = usock_lookup(name);
    if (li < 0) return li;
    struct usock *l = &usocks[li];
    if (l->npend == 8) return -EAGAIN;
    int s = usock_alloc();                              /* the server's end */
    if (s < 0) return -ENFILE;
    usocks[s].state = u->state = US_CONN;
    usocks[s].peer = f->ino;
    usocks[s].pid = l->pid;
    u->peer = s;
    l->pend[l->npend++] = s;
    usock_wake(l);
    return 0;
}

long usock_accept(struct ofile *f, long flags)
{
    struct usock *l = &usocks[f->ino];
    if (l->state != US_LISTEN) return -EINVAL;
    if (!l->npend) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        l->rwait |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    int s = l->pend[0];
    memmove(l->pend, l->pend + 1, --l->npend * sizeof(int));
    long fd = usock_fd(s, flags);
    if (fd < 0) usock_free(s);
    microkit_mr_set(1, 0);                              /* the client has no name */
    return fd;
}

/* send: data at the start of client_buf; nfds descriptors at NET_ADDR_OFF */
long usock_send(struct ofile *f, size_t len, long flags, long nfds)
{
    struct usock *u = &usocks[f->ino];
    if (u->state != US_CONN) return -ENOTCONN;
    if (u->peer_closed) {
        if (!(flags & MSG_NOSIGNAL)) proc_signal(cur, SIGPIPE);
        return -EPIPE;
    }
    struct usock *p = &usocks[u->peer];
    uint32_t space = USOCK_BUF - (p->tail - p->head);
    if (u->dgram && len + 4 > USOCK_BUF) return -EMSGSIZE;
    if (!space || (u->dgram && space < len + 4) || (nfds && p->nfdq + nfds > USOCK_MAXFD)) {
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
        u->wwait |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    const int *fds = (const int *)(client_buf + NET_ADDR_OFF);
    for (long k = 0; k < nfds; k++) {                   /* check before moving anything */
        if (fds[k] < 0 || fds[k] >= NFD || cur->fdt[fds[k]].of < 0) return -EBADF;
    }
    for (long k = 0; k < nfds; k++) {
        int of = cur->fdt[fds[k]].of;
        ofiles[of].refs++;
        p->fdq[p->nfdq++] = of;
    }
    size_t n = len < space ? len : space;
    if (u->dgram) {                                     /* the whole message, with its length in front */
        uint32_t l = len;
        for (int i = 0; i < 4; i++) p->buf[(p->tail + i) % USOCK_BUF] = ((const uint8_t *)&l)[i];
        p->tail += 4;
        n = len;
    }
    for (size_t i = 0; i < n; i++) p->buf[(p->tail + i) % USOCK_BUF] = ((const uint8_t *)client_buf)[i];
    p->tail += n;
    usock_wake(p);
    return n;
}

/* recv: data at the start of client_buf; received descriptors at NET_ADDR_OFF
 * (MR2 = how many; maxfds = how many fit on the process's side) */
long usock_recv(struct ofile *f, size_t len, long flags, long maxfds)
{
    struct usock *u = &usocks[f->ino];
    microkit_mr_set(1, 0);
    microkit_mr_set(2, 0);
    if (u->state != US_CONN) return -ENOTCONN;
    uint32_t avail = u->tail - u->head;
    if (!avail) {
        if (u->peer_closed) return 0;
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
        u->rwait |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    uint32_t skip = 0, msg = avail;
    if (u->dgram) {                                     /* one message; whatever does not fit is lost */
        uint32_t l = 0;
        for (int i = 0; i < 4; i++) ((uint8_t *)&l)[i] = u->buf[(u->head + i) % USOCK_BUF];
        u->head += 4;
        msg = l;
        skip = 0;
    }
    size_t n = len < msg ? len : msg;
    for (size_t i = 0; i < n; i++) ((uint8_t *)client_buf)[i] = u->buf[(u->head + i) % USOCK_BUF];
    if (u->dgram) {
        if (flags & 2) u->head -= 4;                    /* MSG_PEEK: the message stays */
        else u->head += msg;
        (void)skip;
    } else if (!(flags & 2)) {
        u->head += n;                                   /* MSG_PEEK */
    }
    /* Descriptors in transit are delivered with the data (in order) */
    long got = 0;
    int *out = (int *)(client_buf + NET_ADDR_OFF);
    while (maxfds > 0 && got < maxfds && u->nfdq && !(flags & 2)) {
        int of = u->fdq[0];
        memmove(u->fdq, u->fdq + 1, --u->nfdq * sizeof(int));
        int fd = fd_alloc(of, 0, flags & MSG_CMSG_CLOEXEC);
        if (fd < 0) { ofile_release(of); continue; }
        out[got++] = fd;
    }
    microkit_mr_set(2, got);
    if (u->peer >= 0) usock_wake(&usocks[u->peer]);     /* there is room for the writer */
    return n;
}

short usock_poll(struct ofile *f, short events, bool wait)
{
    struct usock *u = &usocks[f->ino];
    short r = 0;
    if (u->state == US_LISTEN) {
        if (u->npend) r |= POLLIN;
    } else if (u->state == US_CONN) {
        if (u->tail != u->head || u->peer_closed) r |= POLLIN;
        if (u->peer_closed) r |= POLLHUP;
        else if (usocks[u->peer].tail - usocks[u->peer].head < USOCK_BUF) r |= POLLOUT;
    } else {
        r |= POLLOUT;
    }
    if (!(r & (events | POLLHUP)) && wait) {
        if (events & POLLIN) u->rwait |= WAIT_BIT(cur);
        if (events & POLLOUT) u->wwait |= WAIT_BIT(cur);
    }
    return r;
}

/* getsockname/getpeername: sockaddr_un at NET_ADDR_OFF of client_buf, MR1 = length */
long usock_addr(struct ofile *f, bool peer)
{
    struct usock *u = &usocks[f->ino];
    if (peer && u->state != US_CONN) return -ENOTCONN;
    const char *name = peer ? (u->peer >= 0 ? usocks[u->peer].name : "") : u->name;
    char *sa = (char *)(client_buf + NET_ADDR_OFF);
    memset(sa, 0, 110);
    sa[0] = AF_UNIX_;
    size_t n = strlen(name);
    if (name[0] == '@') {                               /* abstract: \0 + name, no terminator */
        memcpy(sa + 3, name + 1, n - 1);
        n += 2;
    } else {
        memcpy(sa + 2, name, n + 1);
        n = n ? n + 3 : 2;
    }
    microkit_mr_set(1, n);
    return 0;
}

/* The socket calls on an AF_UNIX socket (vfs.c has checked the descriptor) */
long usock_syscall(long nr, struct ofile *f, long a2, long a3, long a4, long a5, long a6)
{
switch (nr) {
    case __NR_bind:       return usock_bind(f, a3);
    case __NR_listen:     if (usocks[f->ino].state == US_NEW) usocks[f->ino].state = US_LISTEN; return 0;
    case __NR_connect:    return usock_connect(f, a3);
    case __NR_accept:     return usock_accept(f, 0);
    case __NR_accept4:    return usock_accept(f, a4);
    case __NR_sendto:     return usock_send(f, a3, a4, a6);
    case __NR_recvfrom:   return usock_recv(f, a3, a4, a5);
    case __NR_shutdown: {
        struct usock *u = &usocks[f->ino];
        if ((a2 == 1 || a2 == 2) && u->state == US_CONN) {        /* SHUT_WR/RDWR: EOF to the peer */
            usocks[u->peer].peer_closed = true;
            usock_wake(&usocks[u->peer]);
        }
        return 0;
    }
    case __NR_setsockopt: return 0;
    case __NR_getsockopt: {
        struct usock *u = &usocks[f->ino];
        if (a2 == SOL_SOCKET && a3 == SO_PEERCRED) {                 /* struct ucred */
            int cred[3] = { u->peer >= 0 ? usocks[u->peer].pid : 0, 0, 0 };
            memcpy((void *)client_buf, cred, 12);
            microkit_mr_set(1, 12);
            return 0;
        }
        int v = a3 == 3 /* SO_TYPE */ ? 1 : 0;
        memcpy((void *)client_buf, &v, 4);
        microkit_mr_set(1, 4);
        return 0;
    }
    case __NR_getsockname: return usock_addr(f, false);
    case __NR_getpeername: return usock_addr(f, true);
    }
    return -EOPNOTSUPP;
}
