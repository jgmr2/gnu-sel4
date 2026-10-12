/*
 * Translation of POSIX/Linux system calls to seL4.
 *
 * musl (patched) calls __sel4_syscall() instead of executing the `syscall`
 * instruction. Here each call is handled:
 *   - locally, if it only affects this process (memory, TLS, signals), or
 *   - by sending it to posix_server over IPC (seL4_Call), copying the data
 *     behind pointers into the shared buffer.
 *
 * Signals: posix_server accumulates each process's pending signals and
 * delivers them in every reply (message register POSIX_MR_SIGNALS). The
 * handlers and the mask are kept here, and the handler is called before
 * returning from the syscall.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE          /* struct statx, sched_* in musl */
#include <stdint.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <termios.h>
#include <time.h>
#include <poll.h>
#include <sys/select.h>
#include <microkit.h>
#include <sel4posix/protocol.h>
#include <sel4posix/procmem.h>
#include <sel4posix/net_protocol.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <stdlib.h>

#define ARCH_SET_FS 0x1002   /* from Linux <asm/prctl.h> (musl does not ship it) */
#define INPUT_CAP   1        /* the slot's input endpoint (libmicrokit) */

/* Programs do not link libmicrokit: IPC with the seL4 API directly.
 * The slot's POSIX_CHANNEL is the capability BASE_ENDPOINT_CAP + channel. */
#define mr_set(i, v) seL4_SetMR(i, v)
#define mr_get(i)    seL4_GetMR(i)

static uintptr_t brk_cur = PROC_HEAP_BASE, mmap_cur = PROC_MMAP_BASE;
static long my_pid;

/* ------------------------------------------------------------------ */
/* Signals (process-local state)                                      */
/* ------------------------------------------------------------------ */

/* Non-zero while the program is in the runtime (a system call): posix_server then does not
 * interrupt it for a signal, which comes with the call's reply (POSIX_SYS_ASYNC) */
volatile long __posix_in_call;

#define NSIG_ 65
struct k_sigaction {                 /* Linux kernel layout (the one musl uses) */
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned mask[2];
};
static struct k_sigaction sigact[NSIG_];
static uint64_t sig_blocked, sig_pending;

#define SIGBIT(s) (1ULL << ((s) - 1))
#define UNBLOCKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))

/* Calls a handler. It runs in the middle of a call (the reply brought the signal): the shared
 * buffer and the message registers still hold that call's data (what write() is sending, what
 * read() got, an ioctl's results), and the handler's own calls would overwrite them. So they are
 * saved and put back around it (64 KiB, on this stack). If the handler longjmps out, the call is
 * abandoned anyway */
static void __attribute__((noinline)) call_handler(const struct k_sigaction *a, int sig)
{
    char buf[POSIX_BUF_SIZE];
    seL4_Word mrs[POSIX_NUM_MRS];
    memcpy(buf, (void *)PROC_BUF_BASE, sizeof buf);
    for (int i = 0; i < POSIX_NUM_MRS; i++) mrs[i] = mr_get(i);
    if (a->flags & SA_SIGINFO) {
        siginfo_t si;
        memset(&si, 0, sizeof si);
        si.si_signo = sig;
        ((void (*)(int, siginfo_t *, void *))a->handler)(sig, &si, NULL);
    } else {
        a->handler(sig);
    }
    memcpy((void *)PROC_BUF_BASE, buf, sizeof buf);
    for (int i = 0; i < POSIX_NUM_MRS; i++) mr_set(i, mrs[i]);
}

/* Runs the handlers of pending, unblocked signals.
 * Returns 1 if any of them did not ask for SA_RESTART (the syscall must fail with EINTR). */
static int deliver_signals(void)
{
    int interrupt = 0;
    uint64_t ready;
    while ((ready = sig_pending & ~sig_blocked)) {
        int sig = __builtin_ctzll(ready) + 1;
        sig_pending &= ~SIGBIT(sig);
        struct k_sigaction a = sigact[sig];
        if (a.handler == SIG_IGN || a.handler == SIG_DFL) continue;
        if (!(a.flags & SA_RESTART)) interrupt = 1;
        if (a.flags & SA_RESETHAND) sigact[sig].handler = SIG_DFL;
        uint64_t saved = sig_blocked;
        sig_blocked |= ((uint64_t)a.mask[1] << 32 | a.mask[0]);
        if (!(a.flags & SA_NODEFER)) sig_blocked |= SIGBIT(sig);
        sig_blocked &= ~UNBLOCKABLE;
        /* The handler is the program's code: outside the runtime as far as asynchronous signals
         * go (and it may longjmp out, as dash's SIGINT handler does) */
        long depth = __posix_in_call;
        __posix_in_call = 0;
        call_handler(&a, sig);
        __posix_in_call = depth;
        sig_blocked = saved;
    }
    return interrupt;
}

/* ------------------------------------------------------------------ */
/* IPC with the server                                                */
/* ------------------------------------------------------------------ */

static long server_call(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    for (;;) {
        mr_set(0, n);
        mr_set(1, a1);
        mr_set(2, a2);
        mr_set(3, a3);
        mr_set(4, a4);
        mr_set(5, a5);
        mr_set(6, a6);
        seL4_Call(BASE_ENDPOINT_CAP + POSIX_CHANNEL, seL4_MessageInfo_new(0, 0, 0, POSIX_NUM_MRS));
        long r = (long)mr_get(0);
        sig_pending |= mr_get(POSIX_MR_SIGNALS);
        if (r != POSIX_STOPPED) return r;
        seL4_Word badge;                          /* stopped: until SIGCONT */
        seL4_Wait(INPUT_CAP, &badge);
    }
}

/* Extra results of the last call (MR1, MR2...) */
static long server_result(int mr) { return (long)mr_get(mr); }

/* If the call would block (POSIX_RETRY), sleep until the server notifies on
 * the channel and retry. Signals that arrive are delivered; if a handler did
 * not ask for SA_RESTART, the call ends with EINTR. */
static long server_call_blocking6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    for (;;) {
        long r = server_call(n, a1, a2, a3, a4, a5, a6);
        int interrupt = (sig_pending & ~sig_blocked) ? deliver_signals() : 0;
        if (r != POSIX_RETRY && r != -EINTR) return r;
        if (interrupt) return -EINTR;            /* a handler without SA_RESTART */
        if (r == -EINTR) continue;               /* signal handled with SA_RESTART: retry */
        seL4_Word badge;
        seL4_Wait(INPUT_CAP, &badge);
    }
}

static long server_call_blocking5(long n, long a1, long a2, long a3, long a4, long a5)
{
    return server_call_blocking6(n, a1, a2, a3, a4, a5, 0);
}

static long server_call_blocking(long n, long a1, long a2, long a3, long a4)
{
    return server_call_blocking6(n, a1, a2, a3, a4, 0, 0);
}

/* ------------------------------------------------------------------ */
/* I/O                                                                */
/* ------------------------------------------------------------------ */

/* write(): the data is copied into the shared buffer in chunks of up to 64 KiB.
 * If the destination takes only part of it, carry on with the rest (memmove: the
 * source may be the shared buffer itself, see sys_writev). */
static long sys_write(long fd, const char *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done < POSIX_BUF_SIZE ? len - done : POSIX_BUF_SIZE;
        memmove((void *)PROC_BUF_BASE, data + done, chunk);
        long r = server_call_blocking(SYS_write, fd, 0, chunk, 0);
        if (r < 0) return done ? (long)done : r;
        if (r == 0) break;
        done += r;
    }
    return done;
}

/* writev(): everything is gathered into the buffer (musl's stdio always uses writev) */
static long sys_writev(long fd, const struct iovec *iov, int iovcnt)
{
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        size_t n = iov[i].iov_len;
        if (total + n > POSIX_BUF_SIZE) {
            /* It does not fit: send what was gathered, then the rest with write() */
            long r = total ? sys_write(fd, (const char *)PROC_BUF_BASE, total) : 0;
            if (r < 0) return r;
            for (; i < iovcnt; i++) {
                long w = sys_write(fd, iov[i].iov_base, iov[i].iov_len);
                if (w < 0) return r ? r : w;
                r += w;
            }
            return r;
        }
        memcpy((char *)PROC_BUF_BASE + total, iov[i].iov_base, n);
        total += n;
    }
    return sys_write(fd, (const char *)PROC_BUF_BASE, total);
}

/* read(): the server leaves the data in the shared buffer */
static long sys_read(long fd, char *dst, size_t len)
{
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    long r = server_call_blocking(SYS_read, fd, 0, len, 0);
    if (r > 0) memcpy(dst, (void *)PROC_BUF_BASE, r);
    return r;
}

/* readv(): musl's stdio reads with readv (user buffer + FILE buffer) */
static long sys_readv(long fd, const struct iovec *iov, int iovcnt)
{
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) total += iov[i].iov_len;
    if (total > POSIX_BUF_SIZE) total = POSIX_BUF_SIZE;
    long r = server_call_blocking(SYS_read, fd, 0, total, 0);
    if (r <= 0) return r;
    size_t off = 0;
    for (int i = 0; i < iovcnt && off < (size_t)r; i++) {
        size_t n = iov[i].iov_len < (size_t)r - off ? iov[i].iov_len : (size_t)r - off;
        memcpy(iov[i].iov_base, (char *)PROC_BUF_BASE + off, n);
        off += n;
    }
    return r;
}

/* pread/pwrite: like read/write but at a given offset (to files) */
static long sys_pread(long fd, char *dst, size_t len, long off)
{
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    long r = server_call(SYS_pread64, fd, 0, len, off, 0, 0);
    if (r > 0) memcpy(dst, (void *)PROC_BUF_BASE, r);
    return r;
}

static long sys_pwrite(long fd, const char *src, size_t len, long off)
{
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    memcpy((void *)PROC_BUF_BASE, src, len);
    return server_call(SYS_pwrite64, fd, 0, len, off, 0, 0);
}

/* preadv/pwritev (and their version 2, without flags): one by one with pread/pwrite.
 * With offset -1 (version 2 only) they use the current position. */
static long sys_prwv(int write, long fd, const struct iovec *iov, int iovcnt, long off, long flags)
{
    if (flags) return -EOPNOTSUPP;
    if (off == -1) return write ? sys_writev(fd, iov, iovcnt) : sys_readv(fd, iov, iovcnt);
    long total = 0;
    for (int i = 0; i < iovcnt; i++) {
        size_t done = 0;
        while (done < iov[i].iov_len) {
            long r = write ? sys_pwrite(fd, (const char *)iov[i].iov_base + done, iov[i].iov_len - done, off + total)
                           : sys_pread(fd, (char *)iov[i].iov_base + done, iov[i].iov_len - done, off + total);
            if (r < 0) return total ? total : r;
            if (r == 0) return total;
            done += r;
            total += r;
        }
    }
    return total;
}

/* ioctl(): the terminal ones carry a structure that crosses through the buffer */
#define K_TERMIOS_SIZE 60   /* Linux/musl struct termios */
static long sys_ioctl(long fd, long req, void *arg)
{
    long r;
    switch ((uint32_t)req) {             /* ioctl()'s request is an int: 0x80045430 arrives negative */
    case TIOCGWINSZ:
        r = server_call(SYS_ioctl, fd, req, 0, 0, 0, 0);
        if (r >= 0) memcpy(arg, (void *)PROC_BUF_BASE, sizeof(struct winsize));
        return r;
    case TCGETS:
        r = server_call(SYS_ioctl, fd, req, 0, 0, 0, 0);
        if (r >= 0) memcpy(arg, (void *)PROC_BUF_BASE, K_TERMIOS_SIZE);
        return r;
    case TIOCSWINSZ:
        memcpy((void *)PROC_BUF_BASE, arg, sizeof(struct winsize));
        return server_call(SYS_ioctl, fd, req, 0, 0, 0, 0);
    case TCSETS: case TCSETSW: case TCSETSF:
        memcpy((void *)PROC_BUF_BASE, arg, K_TERMIOS_SIZE);
        return server_call(SYS_ioctl, fd, req, 0, 0, 0, 0);
    case TIOCGPGRP:
        r = server_call(SYS_ioctl, fd, req, 0, 0, 0, 0);
        if (r >= 0) *(int *)arg = server_result(1);
        return r;
    case TIOCSPGRP:
    case FIONBIO:
        return server_call(SYS_ioctl, fd, req, *(int *)arg, 0, 0, 0);
    case NET_SIOC_CONFIG:                         /* network configuration: struct net_cfg */
        memcpy((void *)PROC_BUF_BASE, arg, sizeof(struct net_cfg));
        return server_call(SYS_ioctl, fd, req, 0, 0, 0, 0);
    case FIONREAD:
    case TIOCOUTQ:
    case TIOCGPTN:                                /* pseudo-terminals: its number */
    case TIOCGSID:
        r = server_call(SYS_ioctl, fd, req, 0, 0, 0, 0);
        if (r >= 0) *(int *)arg = server_result(1);
        return r;
    case TIOCSPTLCK:                              /* unlockpt(): the int, by value */
        return server_call(SYS_ioctl, fd, req, *(int *)arg, 0, 0, 0);
    default:
        return server_call(SYS_ioctl, fd, req, (long)arg, 0, 0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Sockets (FreeBSD TCP/IP stack, through posix_server)                */
/* Data at the start of the buffer; the address at NET_ADDR_OFF.        */
/* ------------------------------------------------------------------ */

#define NET_ADDR ((void *)(PROC_BUF_BASE + NET_ADDR_OFF))

static long put_addr(const void *addr, socklen_t len)
{
    if (!addr) return 0;
    if (len > NET_ADDR_MAX) return -EINVAL;
    memcpy(NET_ADDR, addr, len);
    return len;
}

/* Address returned by the last call (length in MR1) */
static void get_addr(void *addr, socklen_t *lenp)
{
    if (!addr || !lenp) return;
    socklen_t n = server_result(1);
    memcpy(addr, NET_ADDR, n < *lenp ? n : *lenp);
    *lenp = n;
}

static long sys_sendto(long fd, const char *data, size_t len, long flags, const void *addr, socklen_t alen)
{
    long al = put_addr(addr, alen);
    if (al < 0) return al;
    size_t done = 0;
    do {
        size_t chunk = len - done < NET_DATA_MAX ? len - done : NET_DATA_MAX;
        memcpy((void *)PROC_BUF_BASE, data + done, chunk);
        if (al) put_addr(addr, alen);
        long r = server_call_blocking5(SYS_sendto, fd, 0, chunk, flags, al);
        if (r < 0) return done ? (long)done : r;
        done += r;
        if ((size_t)r < chunk && (flags & MSG_DONTWAIT)) break;
    } while (done < len);
    return done;
}

static long sys_recvfrom(long fd, char *dst, size_t len, long flags, void *addr, socklen_t *alenp)
{
    if (len > NET_DATA_MAX) len = NET_DATA_MAX;
    long r = server_call_blocking(SYS_recvfrom, fd, 0, len, flags);
    if (r > 0) memcpy(dst, (void *)PROC_BUF_BASE, r);
    if (r >= 0) get_addr(addr, alenp);
    return r;
}

/* sendmsg/recvmsg: the iovecs are gathered into the buffer. Control data: only
 * SCM_RIGHTS (passing descriptors over AF_UNIX); it goes at NET_ADDR_OFF, like the
 * address (a connected local socket carries no address). */
#define NET_FDS ((int *)(PROC_BUF_BASE + NET_ADDR_OFF))
#define NET_MAXFDS (NET_ADDR_MAX / (int)sizeof(int))

static long sys_sendmsg(long fd, const struct msghdr *m, long flags)
{
    size_t total = 0;
    for (size_t i = 0; i < (size_t)m->msg_iovlen; i++) {
        size_t n = m->msg_iov[i].iov_len;
        if (total + n > NET_DATA_MAX) n = NET_DATA_MAX - total;
        memcpy((char *)PROC_BUF_BASE + total, m->msg_iov[i].iov_base, n);
        total += n;
    }
    long nfds = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR((struct msghdr *)m, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (nfds + k > NET_MAXFDS) return -EINVAL;
        memcpy(NET_FDS + nfds, CMSG_DATA(c), k * sizeof(int));
        nfds += k;
    }
    long al = 0;
    if (m->msg_name) {
        if (nfds) return -EINVAL;
        al = put_addr(m->msg_name, m->msg_namelen);
        if (al < 0) return al;
    }
    return server_call_blocking6(SYS_sendto, fd, 0, total, flags, al, nfds);
}

static long sys_recvmsg(long fd, struct msghdr *m, long flags)
{
    size_t total = 0;
    for (size_t i = 0; i < (size_t)m->msg_iovlen; i++) total += m->msg_iov[i].iov_len;
    if (total > NET_DATA_MAX) total = NET_DATA_MAX;
    long maxfds = 0;
    if (m->msg_control && m->msg_controllen > CMSG_LEN(0))
        maxfds = (m->msg_controllen - CMSG_LEN(0)) / sizeof(int);
    if (maxfds > NET_MAXFDS) maxfds = NET_MAXFDS;
    long r = server_call_blocking5(SYS_recvfrom, fd, 0, total, flags, maxfds);
    if (r < 0) return r;
    size_t off = 0;
    for (size_t i = 0; i < (size_t)m->msg_iovlen && off < (size_t)r; i++) {
        size_t n = m->msg_iov[i].iov_len < r - off ? m->msg_iov[i].iov_len : r - off;
        memcpy(m->msg_iov[i].iov_base, (char *)PROC_BUF_BASE + off, n);
        off += n;
    }
    if (m->msg_name) get_addr(m->msg_name, &m->msg_namelen);
    m->msg_flags = 0;
    long nfds = server_result(2);                      /* descriptors received (AF_UNIX) */
    if (nfds > 0 && m->msg_control && m->msg_controllen >= CMSG_LEN(nfds * sizeof(int))) {
        struct cmsghdr *c = (struct cmsghdr *)m->msg_control;
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(nfds * sizeof(int));
        memcpy(CMSG_DATA(c), NET_FDS, nfds * sizeof(int));
        m->msg_controllen = CMSG_SPACE(nfds * sizeof(int));
    } else {
        for (long i = 0; i < nfds; i++) server_call(SYS_close, NET_FDS[i], 0, 0, 0, 0, 0);
        if (nfds > 0) m->msg_flags |= MSG_CTRUNC;
        m->msg_controllen = 0;
    }
    return r;
}

static long sys_sockopt(int set, long fd, long level, long name, void *val, socklen_t *lenp, socklen_t len)
{
    if (set) {
        if (len > NET_DATA_MAX) return -EINVAL;
        memcpy((void *)PROC_BUF_BASE, val, len);
        return server_call(SYS_setsockopt, fd, level, name, 0, len, 0);
    }
    long r = server_call(SYS_getsockopt, fd, level, name, *lenp, 0, 0);
    if (r >= 0) {
        socklen_t n = server_result(1);
        memcpy(val, (void *)PROC_BUF_BASE, n < *lenp ? n : *lenp);
        *lenp = n;
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Paths: they travel as strings in the buffer (the second at POSIX_BUF_PATH2) */
/* ------------------------------------------------------------------ */

#define PATH_MAX_ 4096

static long put_path(int second, const char *path)
{
    if (!path) return -EFAULT;
    size_t len = strnlen(path, PATH_MAX_);
    if (len >= PATH_MAX_) return -ENAMETOOLONG;
    memcpy((char *)PROC_BUF_BASE + (second ? POSIX_BUF_PATH2 : 0), path, len + 1);
    return 0;
}

/* Call with one path */
#define PATH_CALL(path, n, ...) ({                          \
    long _e = put_path(0, (const char *)(path));            \
    _e ? _e : server_call(n, __VA_ARGS__);                  \
})

/* Call with one path that may block (open of a FIFO waits for the other end) */
#define PATH_CALL_BLOCKING(path, n, a1, a2, a3, a4, a5) ({   \
    long _e = put_path(0, (const char *)(path));            \
    _e ? _e : server_call_blocking5(n, a1, a2, a3, a4, a5); \
})

/* Call with two paths */
#define PATH2_CALL(p1, p2, n, ...) ({                       \
    long _e = put_path(0, (const char *)(p1));              \
    if (!_e) _e = put_path(1, (const char *)(p2));          \
    _e ? _e : server_call(n, __VA_ARGS__);                  \
})

/* Copies the data the server left in the buffer back to the program */
static long copy_out(long r, void *dst, size_t size)
{
    if (r >= 0 && dst) memcpy(dst, (void *)PROC_BUF_BASE, size);
    return r;
}

#define K_STAT_SIZE   144     /* Linux x86_64 struct stat */
#define K_STATFS_SIZE 120     /* Linux x86_64 struct statfs */

/* ------------------------------------------------------------------ */
/* Time: posix_server reads the CMOS clock at boot and measures the TSC; */
/* every program receives that data in the auxiliary vector and works   */
/* out the time locally (no IPC)                                        */
/* ------------------------------------------------------------------ */

uint64_t __posix_epoch, __posix_tsc0, __posix_tsc_hz = 1000000000;

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* Nanoseconds since boot */
static uint64_t now_ns(void)
{
    uint64_t d = rdtsc() - __posix_tsc0;
    return d / __posix_tsc_hz * 1000000000ULL + d % __posix_tsc_hz * 1000000000ULL / __posix_tsc_hz;
}

static long sys_clock_gettime(long clk, struct timespec *ts)
{
    uint64_t ns = now_ns();
    if (clk == CLOCK_REALTIME || clk == CLOCK_REALTIME_COARSE) ns += __posix_epoch * 1000000000ULL;
    ts->tv_sec = ns / 1000000000ULL;
    ts->tv_nsec = ns % 1000000000ULL;
    return 0;
}

/* Sleeping: the server blocks the process and its timer wakes it up */
static long sys_nanosleep(long clk, int abs, const struct timespec *req)
{
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) return -EINVAL;
    uint64_t target = (uint64_t)req->tv_sec * 1000000000ULL + req->tv_nsec;
    if (abs) {
        if (clk == CLOCK_REALTIME) target -= __posix_epoch * 1000000000ULL;
    } else {
        target += now_ns();
    }
    for (;;) {
        uint64_t now = now_ns();
        if (now >= target) return 0;
        long r = server_call(SYS_nanosleep, target - now, 0, 0, 0, 0, 0);
        if ((sig_pending & ~sig_blocked) && deliver_signals()) return -EINTR;
        if (r == POSIX_RETRY) {
            seL4_Word badge;
            seL4_Wait(INPUT_CAP, &badge);
        }
    }
}

/* poll(): the array travels through the buffer; the timeout is recomputed on every retry */
static long sys_poll(struct pollfd *fds, unsigned long nfds, int64_t timeout_ns)
{
    if (nfds * sizeof(struct pollfd) > POSIX_BUF_SIZE) return -EINVAL;
    uint64_t deadline = timeout_ns > 0 ? now_ns() + timeout_ns : 0;
    for (;;) {
        int64_t rem = timeout_ns;
        if (timeout_ns > 0) {
            uint64_t now = now_ns();
            rem = deadline > now ? (int64_t)(deadline - now) : 0;
        }
        memcpy((void *)PROC_BUF_BASE, fds, nfds * sizeof(struct pollfd));
        long r = server_call(SYS_poll, nfds, rem, 0, 0, 0, 0);
        if (r >= 0) {
            memcpy(fds, (void *)PROC_BUF_BASE, nfds * sizeof(struct pollfd));
            return r;
        }
        int interrupt = (sig_pending & ~sig_blocked) ? deliver_signals() : 0;
        if (r != POSIX_RETRY && r != -EINTR) return r;
        if (interrupt || r == -EINTR) return -EINTR;     /* poll is not restarted (like Linux) */
        seL4_Word badge;
        seL4_Wait(INPUT_CAP, &badge);
    }
}

/* ------------------------------------------------------------------ */
/* epoll on top of poll(): the interest set lives here, in the process; */
/* the server only hands out the descriptor (a placeholder). EPOLLET is  */
/* treated as level-triggered (programs that read until EAGAIN still work). */
/* ------------------------------------------------------------------ */

struct ep_item { int fd; uint32_t events; epoll_data_t data; };
static struct epoll_set { int epfd, n, cap; struct ep_item *it; } *epolls;
static int nepolls;

static struct epoll_set *ep_get(int epfd)
{
    for (int i = 0; i < nepolls; i++) if (epolls[i].epfd == epfd) return &epolls[i];
    return NULL;
}

static void ep_forget(int fd)
{
    struct epoll_set *e = ep_get(fd);
    if (e) { free(e->it); *e = epolls[--nepolls]; }
    for (int i = 0; i < nepolls; i++)                  /* closing an fd removes it from the sets */
        for (int j = 0; j < epolls[i].n; j++)
            if (epolls[i].it[j].fd == fd) epolls[i].it[j--] = epolls[i].it[--epolls[i].n];
}

static long sys_epoll_create(long flags)
{
    long fd = server_call(SYS_epoll_create1, flags, 0, 0, 0, 0, 0);
    if (fd < 0) return fd;
    struct epoll_set *n = realloc(epolls, (nepolls + 1) * sizeof *epolls);
    if (!n) return -ENOMEM;
    epolls = n;
    epolls[nepolls++] = (struct epoll_set){ .epfd = fd };
    return fd;
}

static long sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev)
{
    struct epoll_set *e = ep_get(epfd);
    if (!e) return -EINVAL;
    if (fd == epfd) return -EINVAL;
    int i = 0;
    while (i < e->n && e->it[i].fd != fd) i++;
    switch (op) {
    case EPOLL_CTL_ADD:
        if (i < e->n) return -EEXIST;
        if (e->n == e->cap) {
            int cap = e->cap ? e->cap * 2 : 16;
            struct ep_item *it = realloc(e->it, cap * sizeof *it);
            if (!it) return -ENOMEM;
            e->it = it;
            e->cap = cap;
        }
        e->it[e->n++] = (struct ep_item){ fd, ev->events, ev->data };
        return 0;
    case EPOLL_CTL_MOD:
        if (i == e->n) return -ENOENT;
        e->it[i].events = ev->events;
        e->it[i].data = ev->data;
        return 0;
    case EPOLL_CTL_DEL:
        if (i == e->n) return -ENOENT;
        e->it[i] = e->it[--e->n];
        return 0;
    }
    return -EINVAL;
}

static long sys_poll(struct pollfd *fds, unsigned long nfds, int64_t timeout_ns);

static long sys_epoll_wait(int epfd, struct epoll_event *out, int max, int64_t timeout_ns)
{
    struct epoll_set *e = ep_get(epfd);
    if (!e) return -EINVAL;
    if (max <= 0) return -EINVAL;
    struct pollfd *pf = malloc((e->n ? e->n : 1) * sizeof *pf);
    if (!pf) return -ENOMEM;
    int np = 0;
    for (int i = 0; i < e->n; i++) {
        uint32_t ev = e->it[i].events & (EPOLLIN | EPOLLOUT | EPOLLPRI);
        if (!(e->it[i].events & (EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLERR | EPOLLHUP))) continue;  /* ONESHOT already fired */
        pf[np++] = (struct pollfd){ .fd = e->it[i].fd, .events = ev };
    }
    long r = sys_poll(pf, np, timeout_ns);
    long k = 0;
    if (r > 0) {
        for (int i = 0, p = 0; i < e->n && k < max; i++) {
            struct ep_item *it = &e->it[i];
            if (p >= np || pf[p].fd != it->fd) continue;
            short rev = pf[p++].revents;
            uint32_t ev = (rev & (POLLIN | POLLOUT | POLLPRI)) & it->events;
            ev |= rev & (POLLERR | POLLHUP);
            if (rev & POLLHUP) ev |= it->events & EPOLLRDHUP;
            if (!ev) continue;
            out[k].events = ev;
            out[k].data = it->data;
            k++;
            if (it->events & EPOLLONESHOT) it->events &= EPOLLONESHOT | EPOLLET;
        }
    }
    free(pf);
    return r < 0 ? r : k;
}

/* timerfd: the server uses relative timeouts in ns */
static long sys_timerfd_settime(int fd, int flags, const struct itimerspec *nv, struct itimerspec *old)
{
    int64_t value = nv->it_value.tv_sec * 1000000000LL + nv->it_value.tv_nsec;
    int64_t interval = nv->it_interval.tv_sec * 1000000000LL + nv->it_interval.tv_nsec;
    if (value && (flags & TFD_TIMER_ABSTIME)) {           /* absolute -> relative (the process's clock) */
        struct timespec now;
        sys_clock_gettime(CLOCK_REALTIME, &now);          /* TFD_TIMER_ABSTIME is usually used with REALTIME */
        int64_t n = now.tv_sec * 1000000000LL + now.tv_nsec;
        value = value > n ? value - n : 1;
    }
    long r = server_call(SYS_timerfd_settime, fd, value, interval, 0, 0, 0);
    if (r == 0 && old) {
        uint64_t v = server_result(1), i = server_result(2);
        old->it_value = (struct timespec){ v / 1000000000ULL, v % 1000000000ULL };
        old->it_interval = (struct timespec){ i / 1000000000ULL, i % 1000000000ULL };
    }
    return r;
}

static long sys_timerfd_gettime(int fd, struct itimerspec *cur)
{
    long r = server_call(SYS_timerfd_gettime, fd, 0, 0, 0, 0, 0);
    if (r == 0) {
        uint64_t v = server_result(1), i = server_result(2);
        cur->it_value = (struct timespec){ v / 1000000000ULL, v % 1000000000ULL };
        cur->it_interval = (struct timespec){ i / 1000000000ULL, i % 1000000000ULL };
    }
    return r;
}

/* getrandom(): from the processor's generator (RDRAND); without it, a TSC mix (not cryptographic) */
static uint64_t rand64(void)
{
    unsigned long long v;
    for (int i = 0; i < 10; i++) {
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) return v;
    }
    static uint64_t x;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    x ^= ((uint64_t)hi << 32 | lo) + 0x9e3779b97f4a7c15ULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return x;
}

static long sys_getrandom(void *buf, size_t len)
{
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i += 8) {
        uint64_t v = rand64();
        memcpy(p + i, &v, len - i < 8 ? len - i : 8);
    }
    return len;
}

/* select() on top of poll() */
static long sys_select(int n, fd_set *rd, fd_set *wr, fd_set *ex, int64_t timeout_ns)
{
    struct pollfd pf[FD_SETSIZE];
    int cnt = 0;
    if (n > FD_SETSIZE) n = FD_SETSIZE;
    for (int fd = 0; fd < n; fd++) {
        short ev = (rd && FD_ISSET(fd, rd) ? POLLIN : 0) | (wr && FD_ISSET(fd, wr) ? POLLOUT : 0);
        if (ex && FD_ISSET(fd, ex)) ev |= POLLPRI;
        if (ev) { pf[cnt].fd = fd; pf[cnt].events = ev; pf[cnt].revents = 0; cnt++; }
    }
    long r = sys_poll(pf, cnt, timeout_ns);
    if (r < 0) return r;
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);
    long total = 0;
    for (int i = 0; i < cnt; i++) {
        if (pf[i].revents & POLLNVAL) return -EBADF;
        if (rd && (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) && (pf[i].events & POLLIN)) { FD_SET(pf[i].fd, rd); total++; }
        if (wr && (pf[i].revents & (POLLOUT | POLLERR)) && (pf[i].events & POLLOUT)) { FD_SET(pf[i].fd, wr); total++; }
    }
    return total;
}

static int64_t ts_ns(const struct timespec *ts) { return ts ? ts->tv_sec * 1000000000LL + ts->tv_nsec : -1; }

/* ------------------------------------------------------------------ */
/* Memory and TLS                                                     */
/* ------------------------------------------------------------------ */

/* New memory is handed out zeroed, as Linux does (malloc relies on it): pages the
 * process never touched or gave back to the server show up zeroed. */
#define PAGE_UP(a) (((a) + 0xfff) & ~0xfffUL)

static void release(uintptr_t addr, size_t len)
{
    if (len) server_call(SYS_munmap, addr, len, 0, 0, 0, 0);
}

static long sys_brk(uintptr_t addr)
{
    if (addr >= PROC_HEAP_BASE && addr <= PROC_HEAP_BASE + PROC_HEAP_SIZE) {
        if (addr < brk_cur) {                    /* shrinking: clear the partial page, give back the rest */
            memset((void *)addr, 0, PAGE_UP(addr) - addr);
            release(PAGE_UP(addr), PAGE_UP(brk_cur) - PAGE_UP(addr));
        }
        brk_cur = addr;
    }
    return brk_cur;
}

/* Anonymous mmap: pages of the PROC_MMAP region tracked with a bitmap; munmap gives them back.
 * mmap_cur is the highest mark in use (fork copies up to there). */
#define MMAP_PAGES (PROC_MMAP_SIZE / 4096)
static uint8_t mmap_map[MMAP_PAGES / 8];
#define PG_USED(i) (mmap_map[(i) / 8] & (1u << ((i) % 8)))

static void pages_mark(size_t first, size_t n, int used)
{
    for (size_t i = first; i < first + n; i++) {
        if (used) mmap_map[i / 8] |= 1u << (i % 8);
        else mmap_map[i / 8] &= ~(1u << (i % 8));
    }
    size_t top = (mmap_cur - PROC_MMAP_BASE) / 4096;
    if (used && first + n > top) top = first + n;
    while (top > 0 && !PG_USED(top - 1)) top--;
    mmap_cur = PROC_MMAP_BASE + top * 4096;
}

static long sys_munmap(uintptr_t addr, size_t len);

/* mmap: anonymous (pages on demand), shared memfd (the server maps the pages) or
 * file (a private copy of its contents; shared read-only) */
static long sys_mmap(uintptr_t addr, size_t len, int prot, int flags, int fd, uint64_t off)
{
    int anon = flags & MAP_ANONYMOUS;
    if (!len) return -EINVAL;
    if (!anon && (off & 0xfff)) return -EINVAL;
    size_t n = (len + 0xfff) / 4096, first;
    if (flags & MAP_FIXED) {
        if (addr < PROC_MMAP_BASE || addr + n * 4096 > PROC_MMAP_BASE + PROC_MMAP_SIZE || (addr & 0xfff))
            return -ENOMEM;
        first = (addr - PROC_MMAP_BASE) / 4096;
    } else {
        size_t run = 0;
        for (first = 0; first < MMAP_PAGES; first++) {         /* first hole that is big enough */
            run = PG_USED(first) ? 0 : run + 1;
            if (run == n) break;
        }
        if (run < n) return -ENOMEM;
        first -= n - 1;
    }
    pages_mark(first, n, 1);
    uintptr_t r = PROC_MMAP_BASE + first * 4096;
    if (anon) {
        if (flags & MAP_FIXED) release(r, n * 4096);  /* replaces whatever was there: zeroed pages */
        return r;
    }
    if (flags & MAP_SHARED) {
        long e = server_call(SYS_mmap, r, n * 4096, prot, flags, fd, off);
        if (e == 0) return r;
        if (e != -ENODEV || (prot & PROT_WRITE)) {   /* shared writable file: no */
            pages_mark(first, n, 0);
            return e;
        }
    }
    release(r, n * 4096);
    for (size_t done = 0; done < len; ) {            /* copy of the contents; the rest stays zeroed */
        long k = sys_pread(fd, (char *)r + done, len - done, off + done);
        if (k < 0) {
            sys_munmap(r, n * 4096);
            return k;
        }
        if (k == 0) break;
        done += k;
    }
    return r;
}

static long sys_munmap(uintptr_t addr, size_t len)
{
    if (addr & 0xfff) return -EINVAL;
    if (addr < PROC_MMAP_BASE || addr >= PROC_MMAP_BASE + PROC_MMAP_SIZE) return 0;
    size_t first = (addr - PROC_MMAP_BASE) / 4096, n = (len + 0xfff) / 4096;
    if (first + n > MMAP_PAGES) n = MMAP_PAGES - first;
    pages_mark(first, n, 0);
    release(PROC_MMAP_BASE + first * 4096, n * 4096);
    return 0;
}

static long set_fs_base(uintptr_t base)
{
    /* seL4 x86_64 is configured with FSGSBASE_INST: wrfsbase in user mode */
    __asm__ volatile("wrfsbase %0" :: "r"(base) : "memory");
    return 0;
}

static uintptr_t get_fs_base(void)
{
    uintptr_t base;
    __asm__ volatile("rdfsbase %0" : "=r"(base));
    return base;
}

/* ------------------------------------------------------------------ */
/* Processes                                                          */
/* ------------------------------------------------------------------ */

/* fork(): the server copies this slot's used memory into another slot and starts
 * the child at fork_child_entry, which jumps (longjmp) back here. */
static jmp_buf fork_ctx;

static _Noreturn void fork_child_entry(void)
{
    longjmp(fork_ctx, 1);
}

static __attribute__((noinline)) long sys_fork(void)
{
    if (setjmp(fork_ctx)) {
        my_pid = 0;                 /* child: ask for the pid again */
        return 0;
    }
    uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
    long r = server_call(SYS_fork, brk_cur, mmap_cur, sp, (long)fork_child_entry, get_fs_base(), 0);
    return r;
}

/* execve(): path, argv and envp are packed into the buffer:
 *   [path\0 ... up to POSIX_BUF_PATH2] [argc:u32][envc:u32][argv strings\0...][envp strings\0...] */
static long sys_execve(const char *path, char *const argv[], char *const envp[])
{
    long r = put_path(0, path);
    if (r) return r;
    char *p = (char *)PROC_BUF_BASE + POSIX_BUF_PATH2 + 8, *end = (char *)PROC_BUF_BASE + POSIX_BUF_SIZE;
    uint32_t argc = 0, envc = 0;
    for (; argv && argv[argc]; argc++) {
        size_t n = strlen(argv[argc]) + 1;
        if (p + n > end) return -E2BIG;
        memcpy(p, argv[argc], n);
        p += n;
    }
    for (; envp && envp[envc]; envc++) {
        size_t n = strlen(envp[envc]) + 1;
        if (p + n > end) return -E2BIG;
        memcpy(p, envp[envc], n);
        p += n;
    }
    uint32_t *counts = (uint32_t *)((char *)PROC_BUF_BASE + POSIX_BUF_PATH2);
    counts[0] = argc;
    counts[1] = envc;
    return server_call(SYS_execve, 0, 0, 0, 0, 0, 0);   /* if it works, it does not return */
}

static long sys_wait4(long pid, int *status, long options, struct rusage *ru)
{
    long r = server_call_blocking(SYS_wait4, pid, 0, options, 0);
    if (r > 0 && status) *status = server_result(1);
    if (r >= 0 && ru) memset(ru, 0, sizeof *ru);
    return r;
}

_Noreturn static void sys_exit(long code)
{
    server_call(SYS_exit_group, code, 0, 0, 0, 0, 0);
    for (;;) seL4_Wait(INPUT_CAP, NULL);         /* the server has already stopped this slot */
}

static long getpid_cached(void)
{
    if (!my_pid) my_pid = server_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
    return my_pid;
}

/* ------------------------------------------------------------------ */
/* Signals: calls                                                     */
/* ------------------------------------------------------------------ */

static long sys_sigaction(int sig, const struct k_sigaction *act, struct k_sigaction *old)
{
    if (sig < 1 || sig >= NSIG_) return -EINVAL;
    if (act && (sig == SIGKILL || sig == SIGSTOP)) return -EINVAL;
    if (old) *old = sigact[sig];
    if (act) {
        sigact[sig] = *act;
        /* The server only needs to know what to do on arrival: 0 = default, 1 = ignore, 2 = handle */
        long disp = act->handler == SIG_DFL ? 0 : act->handler == SIG_IGN ? 1 : 2;
        server_call(SYS_rt_sigaction, sig, disp, 0, 0, 0, 0);
        if (disp == 1) sig_pending &= ~SIGBIT(sig);
    }
    return 0;
}

static long sys_sigprocmask(int how, const uint64_t *set, uint64_t *old)
{
    if (old) *old = sig_blocked;
    if (set) {
        switch (how) {
        case SIG_BLOCK:   sig_blocked |= *set; break;
        case SIG_UNBLOCK: sig_blocked &= ~*set; break;
        case SIG_SETMASK: sig_blocked = *set; break;
        default: return -EINVAL;
        }
        sig_blocked &= ~UNBLOCKABLE;
        deliver_signals();          /* whatever got unblocked is delivered now */
    }
    return 0;
}

static long sys_sigsuspend(const uint64_t *mask)
{
    uint64_t saved = sig_blocked;
    sig_blocked = *mask & ~UNBLOCKABLE;
    for (;;) {
        if (sig_pending & ~sig_blocked) {
            deliver_signals();
            break;
        }
        long r = server_call(SYS_rt_sigsuspend, 0, 0, 0, 0, 0, 0);
        if (r == POSIX_RETRY && !(sig_pending & ~sig_blocked)) {
            seL4_Word badge;
            seL4_Wait(INPUT_CAP, &badge);
        }
    }
    sig_blocked = saved;
    return -EINTR;
}

/* ------------------------------------------------------------------ */
/* System information (local, no IPC)                                 */
/* ------------------------------------------------------------------ */

/* statx(): built from the usual stat */
static long sys_statx(long dirfd, const char *path, long flags, unsigned mask, struct statx *sx)
{
    (void)mask;
    struct stat st;
    long r = put_path(0, path);
    if (r) return r;
    r = server_call(SYS_newfstatat, dirfd, 0, 0, flags & (AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH), 0, 0);
    if (r < 0) return r;
    memcpy(&st, (void *)PROC_BUF_BASE, sizeof st);
    memset(sx, 0, sizeof *sx);
    sx->stx_mask = STATX_BASIC_STATS;
    sx->stx_blksize = st.st_blksize;
    sx->stx_nlink = st.st_nlink;
    sx->stx_uid = st.st_uid;
    sx->stx_gid = st.st_gid;
    sx->stx_mode = st.st_mode;
    sx->stx_ino = st.st_ino;
    sx->stx_size = st.st_size;
    sx->stx_blocks = st.st_blocks;
    sx->stx_atime.tv_sec = st.st_atim.tv_sec;
    sx->stx_mtime.tv_sec = st.st_mtim.tv_sec;
    sx->stx_ctime.tv_sec = st.st_ctim.tv_sec;
    sx->stx_rdev_major = st.st_rdev >> 8;
    sx->stx_rdev_minor = st.st_rdev & 0xff;
    sx->stx_dev_major = st.st_dev >> 8;
    sx->stx_dev_minor = st.st_dev & 0xff;
    return 0;
}

static long sys_sysinfo(struct sysinfo *si)
{
    memset(si, 0, sizeof *si);
    si->uptime = now_ns() / 1000000000ULL;
    if (server_call(SYS_sysinfo, 0, 0, 0, 0, 0, 0) == 0) {   /* posix_server's memory for processes */
        si->totalram = server_result(1);
        si->freeram = server_result(2);
        si->procs = server_result(3);
    }
    si->mem_unit = 1;
    return 0;
}

/* prctl(): the options that change nothing visible are accepted */
static long sys_prctl(long op, long a2)
{
    switch (op) {
    case PR_SET_NAME: case PR_SET_PDEATHSIG: case PR_SET_DUMPABLE:
    case PR_SET_NO_NEW_PRIVS: case PR_SET_THP_DISABLE: case PR_SET_TIMERSLACK:
        return 0;
    case PR_GET_NAME:
        strcpy((char *)a2, "program");
        return 0;
    case PR_GET_DUMPABLE: case PR_GET_NO_NEW_PRIVS: case PR_GET_THP_DISABLE:
        return 0;
    }
    return -EINVAL;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                           */
/* ------------------------------------------------------------------ */

void __posix_async_entry(void);                   /* crt0.S */

void __posix_runtime_init(void)
{
    brk_cur = PROC_HEAP_BASE;
    mmap_cur = PROC_MMAP_BASE;
    server_call(POSIX_SYS_ASYNC, (long)&__posix_in_call, (long)__posix_async_entry, 0, 0, 0, 0);
}

/* From __posix_async_entry: the server interrupted the program for a signal. Any call brings the
 * pending signals with its reply; then their handlers run */
void __posix_async_deliver(void)
{
    __posix_in_call = 1;
    server_call(SYS_getpid, 0, 0, 0, 0, 0, 0);
    deliver_signals();
    __posix_in_call = 0;
}

static long sel4_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

long __sel4_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    __posix_in_call++;
    long r = sel4_syscall(n, a1, a2, a3, a4, a5, a6);
    __posix_in_call--;
    return r;
}

static long sel4_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    switch (n) {
    /* memory */
    case SYS_brk:            return sys_brk(a1);
    case SYS_mmap:           return sys_mmap(a1, a2, a3, a4, a5, a6);
    case SYS_munmap:         return sys_munmap(a1, a2);
    case SYS_mprotect:       return 0;
    case SYS_madvise:        return 0;
    case SYS_mremap:         return -ENOMEM;
    /* thread / TLS */
    case SYS_arch_prctl:     return a1 == ARCH_SET_FS ? set_fs_base(a2) : -EINVAL;
    case SYS_set_thread_area: return set_fs_base(a1);
    case SYS_set_tid_address:
    case SYS_gettid:
    case SYS_getpid:         return getpid_cached();
    /* processes */
    case SYS_fork:
    case SYS_vfork:          return sys_fork();
    case SYS_execve:         return sys_execve((const char *)a1, (char *const *)a2, (char *const *)a3);
    case SYS_wait4:          return sys_wait4(a1, (int *)a2, a3, (struct rusage *)a4);
    case SYS_exit:
    case SYS_exit_group:     sys_exit(a1);
    case SYS_pipe:
    case SYS_pipe2: {
        long r = server_call(SYS_pipe2, n == SYS_pipe2 ? a2 : 0, 0, 0, 0, 0, 0);
        if (r >= 0) {
            ((int *)a1)[0] = server_result(1);
            ((int *)a1)[1] = server_result(2);
        }
        return r;
    }
    case SYS_kill:
    case SYS_tkill: {
        long r = server_call(SYS_kill, a1, a2, 0, 0, 0, 0);
        deliver_signals();          /* if it was to itself, deliver it now */
        return r;
    }
    case SYS_tgkill: {
        long r = server_call(SYS_kill, a2, a3, 0, 0, 0, 0);
        deliver_signals();
        return r;
    }
    /* signals */
    case SYS_rt_sigaction:   return sys_sigaction(a1, (const struct k_sigaction *)a2, (struct k_sigaction *)a3);
    case SYS_rt_sigprocmask: return sys_sigprocmask(a1, (const uint64_t *)a2, (uint64_t *)a3);
    case SYS_rt_sigsuspend:  return sys_sigsuspend((const uint64_t *)a1);
    case SYS_rt_sigpending:  *(uint64_t *)a1 = sig_pending; return 0;
    case SYS_sigaltstack:    return 0;
    /* limits (posix_server keeps and enforces them: NOFILE, NPROC, AS, STACK); times: no
     * accounting yet */
    case SYS_getrlimit:
    case SYS_setrlimit:
    case SYS_prlimit64: {
        long pid = n == SYS_prlimit64 ? a1 : 0, res = n == SYS_prlimit64 ? a2 : a1;
        const struct rlimit *nv = (const struct rlimit *)(n == SYS_setrlimit ? a2 : n == SYS_prlimit64 ? a3 : 0);
        struct rlimit *old = (struct rlimit *)(n == SYS_getrlimit ? a2 : n == SYS_prlimit64 ? a4 : 0);
        long r = server_call(SYS_prlimit64, pid, res, nv != NULL, nv ? nv->rlim_cur : 0, nv ? nv->rlim_max : 0, 0);
        if (r == 0 && old) {
            old->rlim_cur = server_result(1);
            old->rlim_max = server_result(2);
        }
        return r;
    }
    case SYS_times:          if (a1) memset((void *)a1, 0, 4 * sizeof(long)); return 0;
    case SYS_getrusage:      if (a2) memset((void *)a2, 0, sizeof(struct rusage)); return 0;
    /* system information */
    case SYS_chown:
    case SYS_lchown:         return PATH_CALL(a1, n, 0, a2, a3, 0, 0, 0);
    case SYS_fchownat:       return PATH_CALL(a2, n, a1, 0, a3, a4, a5, 0);
    case SYS_setitimer:
    case SYS_getitimer: {
        /* struct itimerval: {interval, value}, each {sec, µs} */
        const struct itimerval *nv = n == SYS_setitimer ? (const struct itimerval *)a2 : NULL;
        struct itimerval *old = (struct itimerval *)(n == SYS_setitimer ? a3 : a2);
        uint64_t v = nv ? (uint64_t)nv->it_value.tv_sec * 1000000000ULL + nv->it_value.tv_usec * 1000ULL : 0;
        uint64_t iv = nv ? (uint64_t)nv->it_interval.tv_sec * 1000000000ULL + nv->it_interval.tv_usec * 1000ULL : 0;
        long r = server_call(n, a1, v, iv, 0, 0, 0);
        if (r == 0 && old) {
            uint64_t ov = server_result(1), oi = server_result(2);
            old->it_value.tv_sec = ov / 1000000000ULL;
            old->it_value.tv_usec = ov % 1000000000ULL / 1000;
            old->it_interval.tv_sec = oi / 1000000000ULL;
            old->it_interval.tv_usec = oi % 1000000000ULL / 1000;
        }
        return r;
    }
    case SYS_utimensat: {
        long r = put_path(0, a2 ? (const char *)a2 : "");
        if (r) return r;
        if (a3) memcpy((char *)PROC_BUF_BASE + POSIX_BUF_PATH2, (void *)a3, 2 * sizeof(struct timespec));
        return server_call(n, a1, 0, a3 != 0, a4, 0, 0);
    }
    case SYS_statx:          return sys_statx(a1, (const char *)a2, a3, a4, (struct statx *)a5);
    case SYS_sysinfo:        return sys_sysinfo((struct sysinfo *)a1);
    case SYS_prctl:          return sys_prctl(a1, a2);
    case SYS_sched_getaffinity:                               /* one CPU */
        if ((size_t)a2 < sizeof(long)) return -EINVAL;
        memset((void *)a3, 0, a2);
        *(unsigned long *)a3 = 1;
        return sizeof(long);
    case SYS_sched_setaffinity: return 0;
    /* credentials: the IDs come back in MR1..MR3, the group lists cross through the buffer */
    case SYS_getresuid:
    case SYS_getresgid: {
        long r = server_call(n, 0, 0, 0, 0, 0, 0);
        if (r == 0) {
            *(uint32_t *)a1 = server_result(1);
            *(uint32_t *)a2 = server_result(2);
            *(uint32_t *)a3 = server_result(3);
        }
        return r;
    }
    case SYS_getgroups: {
        long r = server_call(n, a1, 0, 0, 0, 0, 0);
        if (r > 0 && a1 > 0) memcpy((void *)a2, (void *)PROC_BUF_BASE, r * sizeof(uint32_t));
        return r;
    }
    case SYS_setgroups:
        if (a1 < 0 || (size_t)a1 * sizeof(uint32_t) > POSIX_BUF_SIZE) return -EINVAL;
        if (a1) memcpy((void *)PROC_BUF_BASE, (void *)a2, a1 * sizeof(uint32_t));
        return server_call(n, a1, 0, 0, 0, 0, 0);
    case SYS_sched_yield:    seL4_Yield(); return 0;
    case SYS_fadvise64:      return 0;                        /* only advice */
    /* Not implemented yet, with the error programs expect in order to use their fallback
     * (without going through the server, which would report them as missing):
     *   timer_create: coreutils/vim use alarm/setitimer; socket: musl reads /etc/passwd without nscd */
    case SYS_timer_create:   return -ENOSYS;
    /* sockets */
    case SYS_socket:         return server_call(SYS_socket, a1, a2, a3, 0, 0, 0);
    case SYS_socketpair: {
        long r = server_call(SYS_socketpair, a1, a2, a3, 0, 0, 0);
        if (r == 0) { ((int *)a4)[0] = server_result(1); ((int *)a4)[1] = server_result(2); }
        return r;
    }
    case SYS_bind:           { long al = put_addr((void *)a2, a3); return al < 0 ? al : server_call(SYS_bind, a1, 0, al, 0, 0, 0); }
    case SYS_connect:        { long al = put_addr((void *)a2, a3); return al < 0 ? al : server_call_blocking(SYS_connect, a1, 0, al, 0); }
    case SYS_listen:         return server_call(SYS_listen, a1, a2, 0, 0, 0, 0);
    case SYS_accept:
    case SYS_accept4: {
        long r = server_call_blocking(SYS_accept4, a1, 0, 0, n == SYS_accept4 ? a4 : 0);
        if (r >= 0) get_addr((void *)a2, (socklen_t *)a3);
        return r;
    }
    case SYS_sendto:         return sys_sendto(a1, (const char *)a2, a3, a4, (const void *)a5, a6);
    case SYS_recvfrom:       return sys_recvfrom(a1, (char *)a2, a3, a4, (void *)a5, (socklen_t *)a6);
    case SYS_sendmsg:        return sys_sendmsg(a1, (const struct msghdr *)a2, a3);
    case SYS_recvmsg:        return sys_recvmsg(a1, (struct msghdr *)a2, a3);
    case SYS_shutdown:       return server_call(SYS_shutdown, a1, a2, 0, 0, 0, 0);
    case SYS_setsockopt:     return sys_sockopt(1, a1, a2, a3, (void *)a4, NULL, a5);
    case SYS_getsockopt:     return sys_sockopt(0, a1, a2, a3, (void *)a4, (socklen_t *)a5, 0);
    case SYS_getsockname:
    case SYS_getpeername: {
        long r = server_call(n, a1, 0, 0, 0, 0, 0);
        if (r >= 0) get_addr((void *)a2, (socklen_t *)a3);
        return r;
    }
    /* time */
    case SYS_clock_gettime:  return sys_clock_gettime(a1, (struct timespec *)a2);
    case SYS_clock_getres:   if (a2) { ((struct timespec *)a2)->tv_sec = 0; ((struct timespec *)a2)->tv_nsec = 1; } return 0;
    case SYS_nanosleep:      return sys_nanosleep(CLOCK_MONOTONIC, 0, (const struct timespec *)a1);
    /* waiting on several descriptors */
    case SYS_poll:           return sys_poll((struct pollfd *)a1, a2, (int)a3 < 0 ? -1 : (int64_t)(int)a3 * 1000000);
    case SYS_ppoll:          return sys_poll((struct pollfd *)a1, a2, ts_ns((const struct timespec *)a3));
    case SYS_select: {
        const struct timeval *tv = (const struct timeval *)a5;
        return sys_select(a1, (fd_set *)a2, (fd_set *)a3, (fd_set *)a4,
                          tv ? tv->tv_sec * 1000000000LL + tv->tv_usec * 1000LL : -1);
    }
    case SYS_pselect6:       return sys_select(a1, (fd_set *)a2, (fd_set *)a3, (fd_set *)a4, ts_ns((const struct timespec *)a5));
    case SYS_getrandom:      return sys_getrandom((void *)a1, a2);
    /* epoll, eventfd, timerfd */
    case SYS_epoll_create:   return sys_epoll_create(0);
    case SYS_epoll_create1:  return sys_epoll_create(a1);
    case SYS_epoll_ctl:      return sys_epoll_ctl(a1, a2, a3, (struct epoll_event *)a4);
    case SYS_epoll_wait:
    case SYS_epoll_pwait:    return sys_epoll_wait(a1, (struct epoll_event *)a2, a3, (int)a4 < 0 ? -1 : (int64_t)(int)a4 * 1000000);
    case SYS_epoll_pwait2:   return sys_epoll_wait(a1, (struct epoll_event *)a2, a3, ts_ns((const struct timespec *)a4));
    case SYS_close:          ep_forget(a1); return server_call(SYS_close, a1, 0, 0, 0, 0, 0);
    case SYS_timerfd_settime: return sys_timerfd_settime(a1, a2, (const struct itimerspec *)a3, (struct itimerspec *)a4);
    case SYS_timerfd_gettime: return sys_timerfd_gettime(a1, (struct itimerspec *)a2);
    case SYS_clock_nanosleep: return sys_nanosleep(a1, a2 & TIMER_ABSTIME, (const struct timespec *)a3);
    /* I/O and data: they go to the server */
    case SYS_write:          return sys_write(a1, (const char *)a2, a3);
    case SYS_writev:         return sys_writev(a1, (const struct iovec *)a2, a3);
    case SYS_read:           return sys_read(a1, (char *)a2, a3);
    case SYS_readv:          return sys_readv(a1, (const struct iovec *)a2, a3);
    case SYS_pread64:        return sys_pread(a1, (char *)a2, a3, a4);
    case SYS_pwrite64:       return sys_pwrite(a1, (const char *)a2, a3, a4);
    case SYS_preadv:         return sys_prwv(0, a1, (const struct iovec *)a2, a3, a4, 0);
    case SYS_pwritev:        return sys_prwv(1, a1, (const struct iovec *)a2, a3, a4, 0);
    case SYS_preadv2:        return sys_prwv(0, a1, (const struct iovec *)a2, a3, a4, a6);
    case SYS_pwritev2:       return sys_prwv(1, a1, (const struct iovec *)a2, a3, a4, a6);
    /* files: paths to the server, structures back */
    case SYS_open:           return PATH_CALL_BLOCKING(a1, n, 0, a2, a3, 0, 0);
    case SYS_openat:         return PATH_CALL_BLOCKING(a2, n, a1, 0, a3, a4, 0);
    case SYS_stat:
    case SYS_lstat:          { long r = PATH_CALL(a1, n, 0, 0, 0, 0, 0, 0); return copy_out(r, (void *)a2, K_STAT_SIZE); }
    case SYS_newfstatat:     { long r = PATH_CALL(a2, n, a1, 0, 0, a4, 0, 0); return copy_out(r, (void *)a3, K_STAT_SIZE); }
    case SYS_fstat:          return copy_out(server_call(n, a1, 0, 0, 0, 0, 0), (void *)a2, K_STAT_SIZE);
    case SYS_getdents64: {
        long r = server_call(n, a1, 0, a3 < POSIX_BUF_SIZE ? a3 : POSIX_BUF_SIZE, 0, 0, 0);
        return r > 0 ? copy_out(r, (void *)a2, r) : r;
    }
    case SYS_getcwd: {
        long r = server_call(n, 0, a2, 0, 0, 0, 0);
        return r > 0 ? copy_out(r, (void *)a1, r) : r;
    }
    case SYS_chdir:
    case SYS_rmdir:
    case SYS_unlink:         return PATH_CALL(a1, n, 0, 0, 0, 0, 0, 0);
    case SYS_mknod:          return PATH_CALL(a1, n, 0, a2, a3, 0, 0, 0);
    case SYS_mknodat:        return PATH_CALL(a2, n, a1, 0, a3, a4, 0, 0);
    case SYS_mkdir:
    case SYS_truncate:
    case SYS_access:
    case SYS_chmod:          return PATH_CALL(a1, n, 0, a2, 0, 0, 0, 0);
    case SYS_mkdirat:
    case SYS_unlinkat:
    case SYS_faccessat:
    case SYS_faccessat2:
    case SYS_fchmodat:       return PATH_CALL(a2, n, a1, 0, a3, a4, 0, 0);
    case SYS_readlink:       { long r = PATH_CALL(a1, n, 0, 0, a3, 0, 0, 0); return r > 0 ? copy_out(r, (void *)a2, r) : r; }
    case SYS_readlinkat:     { long r = PATH_CALL(a2, n, a1, 0, 0, a4, 0, 0); return r > 0 ? copy_out(r, (void *)a3, r) : r; }
    case SYS_rename:
    case SYS_symlink:        return PATH2_CALL(a1, a2, n, 0, 0, 0, 0, 0, 0);
    case SYS_renameat:
    case SYS_renameat2:      return PATH2_CALL(a2, a4, n, a1, 0, a3, 0, a5, 0);
    case SYS_symlinkat:      return PATH2_CALL(a1, a3, n, 0, a2, 0, 0, 0, 0);
    case SYS_link:           return PATH2_CALL(a1, a2, n, 0, 0, 0, 0, 0, 0);
    case SYS_linkat:         return PATH2_CALL(a2, a4, n, a1, 0, a3, 0, a5, 0);
    case SYS_statfs:         { long r = PATH_CALL(a1, n, 0, 0, 0, 0, 0, 0); return copy_out(r, (void *)a2, K_STATFS_SIZE); }
    case SYS_mount:          return PATH_CALL(a2, n, 0, 0, a4, 0, 0, 0);    /* the target; the flags */
    case SYS_fstatfs:        return copy_out(server_call(n, a1, 0, 0, 0, 0, 0), (void *)a2, K_STATFS_SIZE);
    case SYS_uname:          return copy_out(server_call(n, 0, 0, 0, 0, 0, 0), (void *)a1, 390);
    case SYS_ioctl:          return sys_ioctl(a1, a2, (void *)a3);
    /* the rest: no pointers (or not translated yet) -> to the server as is */
    default:
        return server_call(n, a1, a2, a3, a4, a5, a6);
    }
}
