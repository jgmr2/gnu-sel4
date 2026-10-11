/*
 * Declarations shared between the posix_server modules:
 *   posix_server.c  call dispatch, init/notified/fault
 *   proc.c          processes: slots, exec, fork, exit, wait, signals, groups
 *   tty.c           terminal over sDDF serial (line discipline, termios)
 *   vfs.c           descriptors, paths, pipes and files (on top of fs_ext2)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sel4posix/protocol.h>
#include <sel4posix/procmem.h>
#include <sel4posix/fs_protocol.h>

/* Linux errno values */
#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EINTR         4
#define EIO           5
#define E2BIG         7
#define ENOEXEC       8
#define EBADF         9
#define ECHILD        10
#define EAGAIN        11
#define ENOMEM        12
#define EACCES        13
#define EFAULT        14
#define EBUSY         16
#define EEXIST        17
#define ENOTDIR       20
#define EISDIR        21
#define EINVAL        22
#define EMFILE        24
#define ENOTTY        25
#define ESPIPE        29
#define EPIPE         32
#define ENFILE        23
#define ENXIO         6
#define ENODEV        19
#define EXDEV         18
#define EFBIG         27
#define ENOSPC        28
#define EMSGSIZE      90
#define ERANGE        34
#define ENAMETOOLONG  36
#define ENOSYS        38
#define ELOOP         40
#define SIGALRM       14

/* Signals the server uses */
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGKILL  9
#define SIGSEGV  11
#define SIGFPE   8
#define SIGBUS   7
#define SIGPIPE  13
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGWINCH 28
#define SIGBIT(s) (1ULL << ((s) - 1))

/* ---------------- processes ---------------- */

#define NPROC 32           /* process records (including zombies) */
#define NFD   64           /* descriptors per process */

enum { P_FREE, P_RUN, P_ZOMBIE };

struct proc {
    int state;
    int pid, ppid, pgid, sid;
    int slot;                       /* slot it runs in (-1 if it is a zombie) */
    int status;                     /* status for wait() once it is a zombie */
    bool orphan;                    /* its parent exited: nobody will wait() */
    uint64_t img_end;               /* end of the loaded image (for fork) */
    char name[32];

    /* files */
    struct { int of; bool cloexec; } fdt[NFD];
    char cwd[FS_PATH_MAX];
    uint32_t umask;

    /* signals: the process's runtime keeps handlers and mask;
     * here only the disposition (to decide when sending) and what is pending */
    uint64_t sig_pending, sig_caught, sig_ignored;

    bool wait_child;                /* blocked in wait4 */
    int fifo_wait;                  /* blocked opening a FIFO: pipe index + 1 (0 = none) */
    bool fifo_wait_write;           /* ...for writing */
    uint32_t fifo_wait_opens;       /* opens of the other end when it started waiting */
    uint64_t deadline;              /* poll/sleep deadline (timer ns; 0 = none) */
    int nice;                       /* -20..19 (informational for now: it does not change the seL4 priority) */
    uint64_t itimer_next;           /* ITIMER_REAL: next SIGALRM (timer ns; 0 = off) */
    uint64_t itimer_interval;       /* ns between expirations (0 = one shot) */

    /* for /proc (procfs.c) */
    uint64_t start_ns;              /* creation (timer ns) */
    uint64_t cpu_us;                /* CPU time consumed (seL4 scheduling context) */
    bool blocked;                   /* waiting in a call (POSIX_RETRY) */
    char exe[256];                  /* program being run */
    char cmdline[256];              /* argv separated by NUL */
    uint32_t cmdline_len;
};

extern struct proc procs[NPROC];
extern struct proc *cur;            /* process that made the current call */
extern uintptr_t client_buf;        /* its shared buffer (in its slot's window) */

#define FS_CH        21             /* SERVER_FS_CHANNEL_ID in meta.py */
#define BRIDGE_CH    22             /* SERVER_BRIDGE_CHANNEL_ID in meta.py (to CLIENT_VMM) */
#define NET_CH       23             /* SERVER_NET_CHANNEL_ID in meta.py (to netstack) */
#define NET_LEASE_CH 24             /* SERVER_LEASE_CHANNEL_ID: netstack obtained/renewed the DHCP lease */
#define PROC_INDEX(p) ((int)((p) - procs))

/* proc.c */
void proc_init(void);
void proc_start_init(void);
bool proc_handles(long nr);
long proc_syscall(long nr, long a1, long a2, long a3, long a4, long a5);
struct proc *proc_by_slot(int slot);
struct proc *proc_by_pid(int pid);
void proc_wake(struct proc *p);
void proc_wake_mask(uint32_t mask);
void proc_signal(struct proc *p, int sig);
void proc_signal_pgrp(int pgid, int sig);
void proc_exit(struct proc *p, int status);
bool proc_fault(int slot, long label);

/* mem.c: slots and their memory, on demand */
extern unsigned long mem_pages_used;
void mem_init(void);
uint64_t mem_total(void);
uint64_t mem_free(void);
bool slot_make(int s);
seL4_CPtr slot_tcb(int s);
void slot_signal(int s);
uintptr_t proc_win(int s, uintptr_t vaddr);    /* address vaddr of the process in slot s */
bool mem_ensure(int s, uintptr_t vaddr, size_t len);
void mem_free_range(int s, uintptr_t vaddr, size_t len);
void mem_free_all(int s);
bool mem_fork(int cs, int ps);
bool mem_fault(int s, uintptr_t addr);
uint32_t mem_slot_pages(int s);            /* pages of the process in slot s */
long mem_shm_new(void);                    /* memfd: returns its number */
void mem_shm_ref(int id);
void mem_shm_unref(int id);
uint64_t mem_shm_size(int id);
long mem_shm_truncate(int id, uint64_t size);
long mem_shm_rw(int id, uint64_t off, void *buf, size_t len, bool write);
long mem_shm_map(int s, int id, uintptr_t vaddr, size_t len, uint64_t off, bool write);
uint64_t mem_slot_consumed_us(int s);      /* CPU consumed since the last query */

/* procfs.c: virtual /proc (for procps: ps, top, free, uptime...) */
enum { PROCFS_NONE, PROCFS_DIR, PROCFS_FILE, PROCFS_LINK };
int  procfs_kind(const char *abs);                                    /* abs starts with /proc */
long procfs_read(const char *abs, uint64_t off, char *dst, size_t len);
long procfs_getdents(const char *abs, uint64_t *pos, char *dst, size_t len);
long procfs_stat(const char *abs, void *kstat);                       /* Linux struct stat */
long procfs_readlink(const char *abs, char *dst, size_t len);
void procfs_account(struct proc *p);                                  /* adds up the CPU it consumed */

/* clock.c */
extern uint64_t clock_epoch, clock_tsc0, clock_tsc_hz;
void clock_init(void);
uint64_t clock_now(void);
uint64_t clock_rdtsc(void);
bool timer_notified(unsigned ch);          /* true if it was the timer channel */
void timer_wait(struct proc *p, uint64_t ns);   /* wake p up in ns */
uint64_t timer_now_ns(void);
void timer_rearm(void);

/* bridge.c: bridge to the Linux VM (include/sel4posix/bridge.h) */
void bridge_init(void);

void bridge_notified(void);
long bridge_open(void);
void bridge_close(int sid);
long bridge_write(int sid, const uint8_t *rec, size_t len);
long bridge_read(int sid, uint8_t *out, size_t len);
int  bridge_poll(int sid, bool want_in, bool want_out, bool wait);

/* tty.c */
void tty_init(void);
void tty_notified(unsigned ch);
long tty_read(size_t len);                 /* data -> client_buf */
long tty_write(const char *data, size_t len);
long tty_ioctl(long req, long arg);        /* structures in client_buf */
int  tty_poll(bool want_in, bool want_out, bool wait);
void tty_puts(const char *s);
void tty_putdec(long v);
void tty_set_fg(int pgid);
int  tty_get_fg(void);

/* vfs.c */
void vfs_init(void);
void net_notified(void);                   /* socket activity (netstack) */
void net_lease_changed(void);              /* new DHCP lease: rewrites /etc/resolv.conf */
long net_info_text(char *out, size_t max); /* /proc/net/dhcp */
uint64_t vfs_timerfd_next(void);           /* nearest timerfd deadline (0 = none) */
void vfs_timerfd_fire(uint64_t now);
void vfs_set_fs_clock(void);
bool vfs_handles(long nr);
long vfs_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6);
void vfs_proc_tty(struct proc *p);         /* 0, 1 and 2 to the terminal */
void vfs_fork(struct proc *child, struct proc *parent);
void vfs_close_all(struct proc *p);
void fifo_cancel_wait(struct proc *p);    /* it stopped waiting to open a FIFO */
void vfs_close_cloexec(struct proc *p);
long vfs_normalize(struct proc *p, const char *path, char *out);
long vfs_lookup(const char *abs, bool follow, uint32_t *mode, uint64_t *size);
long vfs_read_file(uint32_t ino, uint64_t off, void *dst, size_t len);
