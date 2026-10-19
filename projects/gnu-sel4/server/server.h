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
#include <sddf/util/printf.h>          /* sddf_snprintf: the slog() macro */
/* printf formats for uint64_t (<inttypes.h>, which sDDF's libc lacks): long is 32 bits on wasm32 */
#ifndef PRIu64
#if UINTPTR_MAX > 0xffffffffu
#define PRIu64 "lu"
#define PRIx64 "lx"
#else
#define PRIu64 "llu"
#define PRIx64 "llx"
#endif
#endif
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
#define SIGHUP   1
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

#define NPROC 128          /* process records (including zombies); = PROC_SLOTS */
#define NFD   256          /* descriptors per process */
#define NGROUPS_ 32        /* supplementary groups per process */

enum { P_FREE, P_RUN, P_ZOMBIE };

/* Resource limits (Linux numbers) that are enforced; the others are only kept */
#define RLIMIT_STACK_   3
#define RLIMIT_NPROC_   6           /* processes of a (real) user: fork; root is exempt */
#define RLIMIT_NOFILE_  7           /* descriptors: up to NFD */
#define RLIMIT_AS_      9           /* bytes of memory a process may have (its page budget) */
#define RLIM_NLIMITS_   16
#define RLIM_INF        (~0ULL)
#define DEFAULT_NPROC   96          /* of PROC_SLOTS = 128: the rest is root's (and the services') */

struct proc {
    int state;
    int pid, ppid, pgid, sid;
    int slot;                       /* slot it runs in (-1 if it is a zombie) */
    int status;                     /* status for wait() once it is a zombie */
    bool orphan;                    /* its parent exited: nobody will wait() */
    uint64_t img_end;               /* end of the loaded image (for fork) */
    /* The program's pages are read from its file when first touched (proc.c): its inode
     * (0 = everything loaded) and its PT_LOAD segments */
    uint32_t img_ino;
    uint8_t nseg;
    struct { uint64_t vaddr, filesz, memsz, off; } seg[4];
    char name[32];

    /* credentials (cred.c) */
    uint32_t uid, euid, suid, gid, egid, sgid;
    uint32_t groups[NGROUPS_];
    int ngroups;

    /* files */
    struct { int of; bool cloexec; } fdt[NFD];
    char cwd[FS_PATH_MAX];
    uint32_t umask;

    /* signals: the process's runtime keeps handlers and mask;
     * here only the disposition (to decide when sending) and what is pending */
    uint64_t sig_pending, sig_caught, sig_ignored;
    uintptr_t async_ctl, async_entry;   /* POSIX_SYS_ASYNC: its in_call counter and entry point */

    /* resource limits (getrlimit, setrlimit, prlimit64): [resource][0 = soft, 1 = hard] */
    uint64_t rlim[RLIM_NLIMITS_][2];
    bool over_budget;               /* its last page was refused by its budget */

    /* job control: stopped by SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU until SIGCONT (proc.c) */
    bool stopped;
    int stop_sig;
    bool stop_reported, cont_reported;   /* to the parent's wait4 (WUNTRACED, WCONTINUED) */
    bool stop_self;                 /* it stopped itself in the current call: suspend after it */
    bool saved_valid;               /* that call's result, for when it retries after SIGCONT */
    long saved_r;
    bool saved_direct;

    bool wait_child;                /* blocked in wait4 */
    int fifo_wait;                  /* blocked opening a FIFO: pipe index + 1 (0 = none) */
    bool fifo_wait_write;           /* ...for writing */
    uint32_t fifo_wait_opens;       /* opens of the other end when it started waiting */
    uint64_t deadline;              /* poll/sleep deadline (timer ns; 0 = none) */
    int nice;                       /* -20..19; > 0 lowers the seL4 priority (proc.c) */
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
extern bool reply_direct;           /* the call put its data at the program's buffer (POSIX_DIRECT) */
extern uintptr_t client_buf;        /* its shared buffer (in its slot's window) */

#define FS_CH        21             /* SERVER_FS_CHANNEL_ID in meta.py */
#define BRIDGE_CH    22             /* SERVER_BRIDGE_CHANNEL_ID in meta.py (to CLIENT_VMM) */
#define NET_CH       23             /* SERVER_NET_CHANNEL_ID in meta.py (to netstack) */
#define NET_LEASE_CH 24             /* SERVER_LEASE_CHANNEL_ID: netstack obtained/renewed the DHCP lease */
#define PROC_INDEX(p) ((int)((p) - procs))
/* A set of processes waiting for something: bit = process index */
typedef unsigned __int128 waitmask_t;    /* NPROC bits */
#define WAIT_BIT(p) ((waitmask_t)1 << PROC_INDEX(p))

/* proc.c */
void proc_init(void);
void proc_start_init(void);
bool proc_handles(long nr);
long proc_syscall(long nr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5);
struct proc *proc_by_slot(int slot);
struct proc *proc_by_pid(int pid);
void proc_wake(struct proc *p);
void proc_wake_mask(waitmask_t mask);
void proc_signal(struct proc *p, int sig);
void proc_signal_pgrp(int pgid, int sig);
bool proc_page_allowed(int slot);            /* mem.c, before giving a slot one more page */
bool proc_pgrp_orphaned(int pgid);           /* no parent in the session outside the group */
void proc_exit(struct proc *p, int status);
bool proc_fault(int slot, long label);

/* cred.c: user and group IDs, permission checks */
void cred_init_root(struct proc *p);
void cred_copy(struct proc *to, const struct proc *from);
void cred_exec(struct proc *p, uint32_t mode, uint32_t file_uid, uint32_t file_gid);
bool cred_in_group(const struct proc *p, uint32_t gid);
bool cred_may(const struct proc *p, uint32_t mode, uint32_t uid, uint32_t gid, int want);
bool cred_may_real(const struct proc *p, uint32_t mode, uint32_t uid, uint32_t gid, int want);
bool cred_may_signal(const struct proc *p, const struct proc *q);
bool cred_handles(long nr);
long cred_syscall(long nr, int64_t a1, int64_t a2, int64_t a3);

/* mem.c: slots and their memory, on demand */
extern unsigned long mem_pages_used;
void mem_init(void);
uint64_t mem_total(void);
uint64_t mem_free(void);
bool slot_make(int s);
seL4_CPtr slot_tcb(int s);
void slot_signal(int s);
void slot_suspend(int s);                      /* stopped (SIGSTOP) until slot_resume */
void slot_resume(int s);
void slot_halt(int s);                         /* it runs no more: exit (wasm: its worker ends) */
#ifdef __wasm__
long mem_wasm_exec(int s, long ino, uint64_t size, const char *args, size_t args_len, uint32_t argc, uint32_t envc);
#endif
uintptr_t proc_win(int s, uintptr_t vaddr);    /* the shared buffer of slot s (contiguous) */
uint8_t *mem_page_ptr(int s, uintptr_t v);     /* byte v of slot s as the server sees it (page must exist) */
bool mem_copy_in(int s, uintptr_t v, const void *src, size_t n);   /* src NULL: clear */
bool mem_copy_user(int s, uintptr_t v, const void *src, size_t n);
void mem_sync_code(int s, uintptr_t v);
bool mem_copy_from_user(int s, uintptr_t v, void *dst, size_t n);
bool mem_has_page(int s, uintptr_t v);
#define PROC_PRIO_NORMAL 100                   /* below posix_server (150), above the VM (90) */
#define PROC_PRIO_LOW    95                    /* nice > 0 */
void mem_slot_set_prio(int s, int prio);
void proc_apply_nice(struct proc *p);
void proc_image_release(uint32_t ino);         /* before a file is changed: load it in full where it runs */
bool mem_ensure(int s, uintptr_t vaddr, size_t len);
bool mem_buf_ensure(int s, size_t len);        /* the first len bytes of the shared buffer exist */
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
bool vfs_root_ro(void);                                              /* / mounted read-only? */
void procfs_account(struct proc *p);
void procfs_load_tick(void);                                         /* load average sampling */
size_t mem_maps_text(int s, const char *exe, char *out, size_t max); /* /proc/<pid>/maps */
long vfs_fd_describe(struct proc *p, int fd, char *out, size_t max); /* /proc/<pid>/fd/<n> target */                                  /* adds up the CPU it consumed */

/* clock.c */
extern uint64_t clock_epoch, clock_tsc0, clock_tsc_hz;
void clock_init(void);
uint64_t clock_now(void);
uint64_t clock_rdtsc(void);

/* perf.c: counters for /proc/perf (count and TSC cycles) */
struct perf_ctr { uint64_t n, cyc; };
#define PERF_NSYS 512
extern struct perf_ctr perf_sys[PERF_NSYS], perf_fs[32], perf_fault, perf_fork_copy, perf_free_all;
static inline void perf_add(struct perf_ctr *c, uint64_t n, uint64_t t0) { c->n += n; c->cyc += clock_rdtsc() - t0; }
size_t perf_text(char *buf, size_t max);
size_t trace_text(char *dst, size_t max);   /* /proc/trace (make TRACE=name) */
size_t vfs_fs_stats(char *dst, size_t max);                           /* fs_ext2's counters, as text */
bool timer_notified(unsigned ch);          /* true if it was the timer channel */
void timer_wait(struct proc *p, uint64_t ns);   /* wake p up in ns */
uint64_t timer_now_ns(void);
/* The log (log.c): a JSON line; str an optional string field "s", then the event's other fields
 * (JSON, without braces) as printf would make them (sDDF's printf has no vsnprintf: a macro) */
void slog_line(const char *level, const char *ev, const char *str, const char *fields);
#define slog(level, ev, str, ...) do { char slog_f_[256]; sddf_snprintf(slog_f_, sizeof slog_f_, __VA_ARGS__); \
                                       slog_line(level, ev, str, slog_f_); } while (0)
size_t log_text(char *dst, size_t max);
void timer_rearm(void);
extern uint64_t fs_flush_at;                   /* vfs.c: fs_ext2 write-back due (timer ns; 0: nothing) */
long fs_call(long op, uint64_t a1, uint64_t a2, uint64_t a3);

/* bridge.c: bridge to the Linux VM (include/sel4posix/bridge.h) */
void bridge_init(void);

void bridge_notified(void);
long bridge_open(void);
void bridge_close(int sid);
long bridge_write(int sid, const uint8_t *rec, size_t len);
long bridge_read(int sid, uint8_t *out, size_t len);
int  bridge_poll(int sid, bool want_in, bool want_out, bool wait);

/* tty.c: terminals (0 = the console, 1..NTTY-1 = pseudo-terminals /dev/pts/0...) */
#define NTTY 17
void tty_init(void);
void tty_notified(unsigned ch);
long tty_open(const char *abs, uint32_t flags, bool *master);   /* -> tty index or -errno */
void tty_close(int tty, bool master);
int  tty_pts_number(const char *abs);       /* "/dev/pts/<n>" -> n, or -1 */
bool tty_exists(int tty);
long tty_read(int tty, size_t len);          /* data -> client_buf */
long tty_write(int tty, const char *data, size_t len);
long pty_master_read(int tty, size_t len);
long pty_master_write(int tty, const char *data, size_t len);
long tty_ioctl(int tty, bool master, long req, long arg);   /* structures in client_buf */
int  tty_poll(int tty, bool want_in, bool want_out, bool wait);
int  pty_master_poll(int tty, bool want_in, bool want_out, bool wait);
int  tty_ctty(const struct proc *p);         /* its controlling terminal, or -1 */
int  tty_fg_of(int tty);                     /* foreground process group */
void tty_session_leader_exit(const struct proc *p);
void tty_console_session(int sid);         /* (init: 0 = free for the next login) */
uint32_t tty_rdev(int tty);
void tty_name(int tty, char *out, size_t max);
void tty_puts(const char *s);                /* the server's own messages, on the console */
void tty_putdec(long v);

/* vfs.c */
void vfs_init(void);
void net_notified(void);                   /* socket activity (netstack) */
void net_lease_changed(void);              /* new DHCP lease: rewrites /etc/resolv.conf */
long net_info_text(char *out, size_t max); /* /proc/net/dhcp */
uint64_t vfs_timerfd_next(void);           /* nearest timerfd deadline (0 = none) */
void vfs_timerfd_fire(uint64_t now);
void vfs_set_fs_clock(void);
void vfs_unmount(void);                   /* shutdown: sync and mark the file system clean */
bool vfs_handles(long nr);
long vfs_syscall(long nr, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6);
void vfs_proc_tty(struct proc *p);         /* 0, 1 and 2 to the terminal */
void vfs_fork(struct proc *child, struct proc *parent);
void vfs_close_all(struct proc *p);
void fifo_cancel_wait(struct proc *p);    /* it stopped waiting to open a FIFO */
void vfs_close_cloexec(struct proc *p);
long vfs_normalize(struct proc *p, const char *path, char *out);
long vfs_lookup(const char *abs, bool follow, uint32_t *mode, uint64_t *size);
long vfs_read_file(uint32_t ino, uint64_t off, void *dst, size_t len);
long vfs_read_to_proc(uint32_t ino, uint64_t off, int slot, uintptr_t vaddr, size_t len);
long vfs_owner(uint32_t ino, uint32_t *uid, uint32_t *gid);
