/*
 * Internal interface between the parts of posix_server's VFS:
 *   vfs.c       descriptors, paths, permissions, files (fs_ext2) and the call dispatch
 *   vfs_pipe.c  pipes and named pipes (FIFOs)
 *   vfs_evfd.c  eventfd and timerfd
 *   vfs_net.c   network sockets (netstack) and the network configuration
 *   vfs_unix.c  local sockets (AF_UNIX)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <os/sddf.h>
#include <string.h>
#include <sel4posix/fs_protocol.h>
#include <sel4posix/net_protocol.h>
#include <sddf/util/printf.h>
#include "server.h"
#include "syscall_nr.h"

extern uintptr_t fs_buf, net_buf;

#define AT_FDCWD             (-100)
#define AT_SYMLINK_NOFOLLOW  0x100
#define AT_REMOVEDIR         0x200
#define AT_EMPTY_PATH        0x1000

#define O_ACCMODE    03
#define O_RDONLY     00
#define O_WRONLY     01
#define O_RDWR       02
#define O_CREAT      0100
#define O_EXCL       0200
#define O_TRUNC      01000
#define O_APPEND     02000
#define O_NONBLOCK   04000
#define O_DIRECTORY  0200000
#define O_NOFOLLOW   0400000
#define O_CLOEXEC    02000000

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFLNK  0120000
#define S_IFIFO  0010000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7
#define F_DUPFD_CLOEXEC  1030
#define FD_CLOEXEC       1

#define X_OK 1

#define K_STAT_SIZE    144     /* Linux x86_64 struct stat */
#define K_STATFS_SIZE  120     /* Linux x86_64 struct statfs */
#define ST_SIZE_OFFSET 48      /* offsetof(struct stat, st_size) */

/* ---------------- descriptors ---------------- */

enum { OF_FREE, OF_TTY, OF_NULL, OF_FILE, OF_DIR, OF_PIPE_R, OF_PIPE_W, OF_LINUX, OF_SOCKET,
       OF_PROCDIR, OF_PROCFILE,        /* /proc (procfs.c): identified by its path */
       OF_EVENTFD, OF_TIMERFD,         /* counters and timers that are read like files */
       OF_EPOLL,                       /* placeholder: the interest set lives in the runtime */
       OF_USOCK,                       /* local socket (AF_UNIX) */
       OF_SHM,                         /* memfd: shared memory (mem.c) */
       OF_KMSG,                        /* /dev/kmsg: to the serial port (logs, make test) */
       OF_RANDOM,                      /* /dev/urandom, /dev/random (RDRAND) */
       OF_ZERO };                      /* /dev/zero */

/* Is the path under /proc? */
static inline bool is_proc(const char *abs)
{
    return !strncmp(abs, "/proc", 5) && (abs[5] == 0 || abs[5] == '/');
}

struct ofile {
    int refs;
    int type;
    uint32_t ino;               /* file/directory; pipe index; netstack socket */
    uint64_t off;               /* (socket: 1 = blocking connect() in progress) */
    uint32_t flags;
    char path[FS_PATH_MAX];     /* absolute path (for fchdir and the *at calls) */
};

#define NOFILE 512
extern struct ofile ofiles[NOFILE];

/* Pipes: circular buffer; whoever blocks goes into rwait/wwait (bits = process index).
 * A named pipe (mkfifo) is the same object, found by its inode while someone has it open or
 * is blocked opening it; its data goes away with the last close, as in Linux. */
#define NPIPE     128
#define PIPE_SIZE 16384
struct pipe {
    bool used;
    int readers, writers;
    uint32_t head, tail;        /* tail - head = bytes in the buffer */
    waitmask_t rwait, wwait;
    uint32_t fifo;              /* named pipe: its inode (0 = anonymous pipe) */
    waitmask_t owait;             /* processes blocked in open() waiting for the other end */
    uint32_t r_opens, w_opens;  /* opens of each end so far: a waiter returns once the other moved */
    char buf[PIPE_SIZE];
};
extern struct pipe pipes[NPIPE];

/* eventfd: a counter; read empties it (or subtracts 1 with EFD_SEMAPHORE), write adds */
#define NEVFD 64
struct evfd {
    bool used, semaphore;
    uint64_t count;
    waitmask_t rwait, wwait;
};

/* timerfd: deadlines in server timer ns; read returns the expirations */
#define NTFD 64
struct tfd {
    bool used;
    uint64_t next, interval, expirations;
    waitmask_t rwait;
};
extern struct evfd evfds[NEVFD];
extern struct tfd tfds[NTFD];

/* poll(): struct pollfd[] in the buffer */
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
struct k_pollfd { int fd; short events, revents; };

/* sockets (vfs_net.c, vfs_unix.c) */
#define AF_UNIX_      1
#define S_IFSOCK_     0140000
#define WHEEL_GID     10              /* /etc/group: may use the Linux VM (/dev/linux) */
#define FIONREAD     0x541B
#define FIONBIO      0x5421
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000
#define MSG_NOSIGNAL 0x4000
#define MSG_DONTWAIT 0x40
#define SOL_SOCKET   1
#define SO_ERROR     4
#define EINPROGRESS  115
#define EALREADY     114
#define EISCONN      106
#define ENOTSOCK     88
#define EOPNOTSUPP   95
#define POLLOUT_     POLLOUT
#define POLLERR_     POLLERR
#define POLLHUP_     POLLHUP

struct usock;
extern uint32_t last_uid, last_gid;            /* owner of what fs_lookup found last */
long write_file(const char *abs, const char *data, size_t len);   /* as the server (root) */
short evfd_poll(struct ofile *f, short events, bool in, bool out, bool wait);
short tfd_poll(struct ofile *f, bool in, bool wait);
short sock_poll(struct ofile *f, short events, bool wait);
long sock_syscall(long nr, struct ofile *f, long a2, long a3, long a4, long a5, long a6);
long usock_syscall(long nr, struct ofile *f, long a2, long a3, long a4, long a5, long a6);

int fd_alloc(int of, long min, bool cloexec);
int ofile_alloc(int type);
long do_pipe(long flags);
long evfd_new(uint64_t init, long flags);
long evfd_read(struct ofile *f, size_t len);
long evfd_write(struct ofile *f, size_t len);
long fd_close(long fd);
long fifo_open(uint32_t ino, uint32_t flags, const char *abs);
long fs_call(long op, uint64_t a1, uint64_t a2, uint64_t a3);
long fs_lookup(const char *abs, bool follow, uint32_t *mode, uint64_t *size);
long may_change_dir(const char *abs);
long net_call(long op, long a1, long a2, long a3, long a4);
long normalize(long dirfd, const char *path, char *out);
long pipe_read(struct ofile *f, size_t len);
long pipe_write(struct ofile *f, size_t len);
long sock_accept(struct ofile *f, long flags);
long sock_addr_call(long op, struct ofile *f, long alen);
long sock_connect(struct ofile *f, long alen);
long sock_recv(struct ofile *f, size_t len, long flags);
long sock_send(struct ofile *f, size_t len, long flags, long alen);
long sock_socket(long domain, long type, long proto);
long tfd_new(long flags);
long tfd_read(struct ofile *f, size_t len);
long tfd_settime(struct ofile *f, bool set, uint64_t value, uint64_t interval);
long usock_accept(struct ofile *f, long flags);
long usock_addr(struct ofile *f, bool peer);
long usock_bind(struct ofile *f, long alen);
long usock_connect(struct ofile *f, long alen);
long usock_new(long type);
long usock_pair(long type);
long usock_recv(struct ofile *f, size_t len, long flags, long maxfds);
long usock_send(struct ofile *f, size_t len, long flags, long nfds);
short usock_poll(struct ofile *f, short events, bool wait);
void fs_set_path(uint32_t off, const char *abs);
void ofile_release(int of);
void pipe_maybe_free(struct pipe *pp);
void sock_addr_out(void);
void tfd_update(struct tfd *t, uint64_t now);
void usock_free(int i);
void usock_wake(struct usock *u);
