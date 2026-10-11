/*
 * posix_server's virtual /proc, with the files procps-ng reads (ps, top,
 * free, uptime, pgrep, kill...). Everything is generated on read, in Linux format:
 *
 *   /proc/<pid>/{stat,statm,status,cmdline,environ,comm,maps}, exe and cwd (links),
 *   fd/<n> (links to what each descriptor is)
 *   /proc/self -> <pid> of the caller
 *   /proc/{meminfo,stat,uptime,loadavg,version,mounts}, /proc/net/dhcp
 *   /proc/perf: performance counters (perf.c)
 *   /proc/sys/kernel/{osrelease,pid_max,hostname}
 *
 * Memory: posix_server's process memory (mem.c). CPU: the time seL4 accounts
 * in each process's scheduling context. No threads, no swap.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <os/sddf.h>
#include <string.h>
#include <sddf/util/printf.h>
#include "server.h"

#define HZ        100                     /* musl's sysconf(_SC_CLK_TCK) */
#define S_IFDIR   0040000
#define S_IFREG   0100000
#define S_IFLNK   0120000
#define DT_DIR    4
#define DT_REG    8
#define DT_LNK    10

/* The "kernel version" programs see: posix_server mimics the Linux ABI and
 * procps picks the /proc format based on this number */
#define OSRELEASE "6.1.0-gnu-sel4"

static uint64_t total_cpu_us;             /* of all processes since boot */

void procfs_account(struct proc *p)
{
    if (p->state != P_RUN || p->slot < 0) return;
    uint64_t us = mem_slot_consumed_us(p->slot);
    p->cpu_us += us;
    total_cpu_us += us;
}

static void account_all(void)
{
    for (int i = 0; i < NPROC; i++) procfs_account(&procs[i]);
}

/* ---------------- paths ---------------- */

enum {
    F_NONE,
    /* global */
    F_ROOT, F_SELF, F_MEMINFO, F_STAT, F_UPTIME, F_LOADAVG, F_VERSION, F_MOUNTS,
    F_SYS, F_SYS_KERNEL, F_OSRELEASE, F_PIDMAX, F_HOSTNAME, F_NET, F_NET_DHCP, F_PERF,
    /* per process */
    F_PID, F_P_STAT, F_P_STATM, F_P_STATUS, F_P_CMDLINE, F_P_ENVIRON, F_P_COMM, F_P_EXE, F_P_CWD,
    F_P_MAPS, F_P_FD, F_P_FDLINK,
};

static const struct { const char *name; int id, kind, dt; } root_ents[] = {
    { "self", F_SELF, PROCFS_LINK, DT_LNK },
    { "meminfo", F_MEMINFO, PROCFS_FILE, DT_REG },
    { "stat", F_STAT, PROCFS_FILE, DT_REG },
    { "uptime", F_UPTIME, PROCFS_FILE, DT_REG },
    { "loadavg", F_LOADAVG, PROCFS_FILE, DT_REG },
    { "version", F_VERSION, PROCFS_FILE, DT_REG },
    { "mounts", F_MOUNTS, PROCFS_FILE, DT_REG },
    { "perf", F_PERF, PROCFS_FILE, DT_REG },
    { "sys", F_SYS, PROCFS_DIR, DT_DIR },
    { "net", F_NET, PROCFS_DIR, DT_DIR },
}, net_ents[] = {
    { "dhcp", F_NET_DHCP, PROCFS_FILE, DT_REG },
}, kernel_ents[] = {
    { "osrelease", F_OSRELEASE, PROCFS_FILE, DT_REG },
    { "pid_max", F_PIDMAX, PROCFS_FILE, DT_REG },
    { "hostname", F_HOSTNAME, PROCFS_FILE, DT_REG },
}, pid_ents[] = {
    { "stat", F_P_STAT, PROCFS_FILE, DT_REG },
    { "statm", F_P_STATM, PROCFS_FILE, DT_REG },
    { "status", F_P_STATUS, PROCFS_FILE, DT_REG },
    { "cmdline", F_P_CMDLINE, PROCFS_FILE, DT_REG },
    { "environ", F_P_ENVIRON, PROCFS_FILE, DT_REG },
    { "comm", F_P_COMM, PROCFS_FILE, DT_REG },
    { "exe", F_P_EXE, PROCFS_LINK, DT_LNK },
    { "cwd", F_P_CWD, PROCFS_LINK, DT_LNK },
    { "maps", F_P_MAPS, PROCFS_FILE, DT_REG },
    { "fd", F_P_FD, PROCFS_DIR, DT_DIR },
};
#define N(a) (sizeof(a) / sizeof(*(a)))

/* Next path component: copies up to '/' and advances */
static const char *next(const char *p, char *out, size_t max)
{
    while (*p == '/') p++;
    size_t n = 0;
    while (*p && *p != '/') {
        if (n + 1 < max) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return p;
}

static int lookup_fd;                          /* F_P_FDLINK: which descriptor */

/* Identifies abs (which starts with /proc); *pp = the process if it belongs to one */
static int lookup(const char *abs, struct proc **pp, int *kind)
{
    char c[32];
    const char *p = next(abs, c, sizeof c);            /* "proc" */
    *pp = NULL;
    p = next(p, c, sizeof c);
    if (!*c) { *kind = PROCFS_DIR; return F_ROOT; }

    struct proc *proc = NULL;
    if (!strcmp(c, "self") && *p) {
        proc = cur;                                    /* /proc/self/... without going through the link */
    } else if (*c >= '0' && *c <= '9') {
        int pid = 0;
        for (const char *d = c; *d; d++) {
            if (*d < '0' || *d > '9') return F_NONE;
            pid = pid * 10 + (*d - '0');
        }
        proc = proc_by_pid(pid);
        if (!proc) return F_NONE;
    }
    if (proc) {
        *pp = proc;
        p = next(p, c, sizeof c);
        if (!*c) { *kind = PROCFS_DIR; return F_PID; }
        if (!strcmp(c, "fd")) {
            p = next(p, c, sizeof c);
            if (!*c) { *kind = PROCFS_DIR; return F_P_FD; }
            if (*p && p[1]) return F_NONE;
            int fd = 0;
            for (const char *d = c; *d; d++) {
                if (*d < '0' || *d > '9' || fd > NFD) return F_NONE;
                fd = fd * 10 + (*d - '0');
            }
            char tmp[8];
            if (vfs_fd_describe(proc, fd, tmp, sizeof tmp) < 0) return F_NONE;
            lookup_fd = fd;
            *kind = PROCFS_LINK;
            return F_P_FDLINK;
        }
        if (*p && p[1]) return F_NONE;
        for (unsigned i = 0; i < N(pid_ents); i++)
            if (!strcmp(c, pid_ents[i].name)) { *kind = pid_ents[i].kind; return pid_ents[i].id; }
        return F_NONE;
    }
    if (!strcmp(c, "net")) {
        p = next(p, c, sizeof c);
        if (!*c) { *kind = PROCFS_DIR; return F_NET; }
        for (unsigned i = 0; i < N(net_ents); i++)
            if (!strcmp(c, net_ents[i].name)) { *kind = net_ents[i].kind; return net_ents[i].id; }
        return F_NONE;
    }
    if (!strcmp(c, "sys")) {
        p = next(p, c, sizeof c);
        if (!*c) { *kind = PROCFS_DIR; return F_SYS; }
        if (strcmp(c, "kernel")) return F_NONE;
        p = next(p, c, sizeof c);
        if (!*c) { *kind = PROCFS_DIR; return F_SYS_KERNEL; }
        for (unsigned i = 0; i < N(kernel_ents); i++)
            if (!strcmp(c, kernel_ents[i].name)) { *kind = kernel_ents[i].kind; return kernel_ents[i].id; }
        return F_NONE;
    }
    if (*p && p[1]) return F_NONE;
    for (unsigned i = 0; i < N(root_ents); i++)
        if (!strcmp(c, root_ents[i].name)) { *kind = root_ents[i].kind; return root_ents[i].id; }
    return F_NONE;
}

int procfs_kind(const char *abs)
{
    struct proc *p;
    int kind;
    return lookup(abs, &p, &kind) == F_NONE ? PROCFS_NONE : kind;
}

/* ---------------- contents ---------------- */

static char gen[16384];

static char state_of(struct proc *p)
{
    return p->state == P_ZOMBIE ? 'Z' : p->blocked ? 'S' : 'R';
}

static uint64_t ticks(uint64_t us) { return us / (1000000 / HZ); }

static int count_running(int *total, int *maxpid)
{
    int run = 0;
    *total = 0;
    *maxpid = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->state == P_FREE) continue;
        (*total)++;
        if (q->pid > *maxpid) *maxpid = q->pid;
        if (q->state == P_RUN && !q->blocked) run++;
    }
    return run;
}

/* Load average as Linux computes it: every 5 s, avg = avg * e^(-5/60|300|900) + n * (1 - that),
 * in fixed point with 11 bits, n = runnable processes. Sampled from the system call path */
static uint64_t load_avg[3], load_next_ns;
static const uint64_t load_exp[3] = { 1884, 2014, 2037 };        /* 2048 * e^(-5 s / 1, 5, 15 min) */

void procfs_load_tick(void)
{
    uint64_t now = timer_now_ns();
    if (now < load_next_ns) return;
    int total, maxpid, run = count_running(&total, &maxpid);
    uint64_t n = (uint64_t)run << 11;
    int steps = load_next_ns ? (int)((now - load_next_ns) / 5000000000ULL) + 1 : 1;
    if (steps > 200) steps = 200;
    while (steps--)
        for (int k = 0; k < 3; k++) load_avg[k] = (load_avg[k] * load_exp[k] + n * (2048 - load_exp[k])) >> 11;
    load_next_ns = now + 5000000000ULL;
}

static size_t generate(int id, struct proc *p)
{
    size_t n = 0, max = sizeof gen;
    uint64_t now_us = timer_now_ns() / 1000;
#define OUT(...) (n += sddf_snprintf(gen + n, n < max ? max - n : 0, __VA_ARGS__))
    switch (id) {
    case F_MEMINFO: {
        uint64_t total = mem_total() >> 10, free = mem_free() >> 10;
        OUT("MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\n",
            total, free, free);
        OUT("Buffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\n"
            "Shmem:                 0 kB\nSReclaimable:          0 kB\n"
            "SwapTotal:             0 kB\nSwapFree:              0 kB\n");
        break;
    }
    case F_STAT: {
        account_all();
        uint64_t user = ticks(total_cpu_us), up = ticks(now_us);
        uint64_t idle = up > user ? up - user : 0;
        int total, maxpid, run = count_running(&total, &maxpid);
        OUT("cpu  %lu 0 0 %lu 0 0 0 0 0 0\ncpu0 %lu 0 0 %lu 0 0 0 0 0 0\n", user, idle, user, idle);
        OUT("intr 0\nctxt 0\nbtime %lu\nprocesses %d\nprocs_running %d\nprocs_blocked 0\n",
            clock_epoch, maxpid, run);
        break;
    }
    case F_UPTIME: {
        account_all();
        uint64_t idle = now_us > total_cpu_us ? now_us - total_cpu_us : 0;
        OUT("%lu.%02lu %lu.%02lu\n", now_us / 1000000, now_us / 10000 % 100,
            idle / 1000000, idle / 10000 % 100);
        break;
    }
    case F_LOADAVG: {
        int total, maxpid, run = count_running(&total, &maxpid);
        procfs_load_tick();
        for (int k = 0; k < 3; k++)                  /* fixed point, 11 bits, as Linux */
            OUT("%lu.%02lu ", load_avg[k] >> 11, ((load_avg[k] & 2047) * 100) >> 11);
        OUT("%d/%d %d\n", run, total, maxpid);
        break;
    }
    case F_VERSION:   OUT("Linux version " OSRELEASE " (GNU/seL4 posix_server) #1\n"); break;
    case F_MOUNTS:    OUT("/dev/disk1 / ext2 rw 0 0\nproc /proc proc rw 0 0\n"); break;
    case F_OSRELEASE: OUT(OSRELEASE "\n"); break;
    case F_PIDMAX:    OUT("32768\n"); break;
    case F_HOSTNAME:  OUT("gnu-sel4\n"); break;
    case F_NET_DHCP:  n = net_info_text(gen, max); break;
    case F_P_MAPS:    n = p->slot >= 0 ? mem_maps_text(p->slot, p->exe, gen, max) : 0; break;
    case F_PERF:      n = perf_text(gen, max); break;

    case F_P_STAT: {
        procfs_account(p);
        uint64_t pages = p->slot >= 0 ? mem_slot_pages(p->slot) : 0;
        OUT("%d (%s) %c %d %d %d %d %d 4194304 0 0 0 0 %lu 0 0 0 %d %d 1 0 %lu %lu %lu "
            "18446744073709551615 0 0 0 0 0 %lu 0 %lu %lu 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 %d\n",
            p->pid, p->name, state_of(p), p->orphan ? 1 : p->ppid, p->pgid, p->sid,
            0x0501 /* /dev/console (5,1) */, tty_get_fg(),
            ticks(p->cpu_us), 20 + p->nice, p->nice,
            ticks(p->start_ns / 1000), pages * 4096, pages,
            p->sig_pending, p->sig_ignored, p->sig_caught,
            p->state == P_ZOMBIE ? p->status : 0);
        break;
    }
    case F_P_STATM: {
        uint64_t pages = p->slot >= 0 ? mem_slot_pages(p->slot) : 0;
        OUT("%lu %lu 0 0 0 %lu 0\n", pages, pages, pages);
        break;
    }
    case F_P_STATUS: {
        uint64_t kb = (p->slot >= 0 ? mem_slot_pages(p->slot) : 0) * 4;
        char st = state_of(p);
        OUT("Name:\t%s\nUmask:\t%04o\nState:\t%c (%s)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\n"
            "TracerPid:\t0\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t%d\nGroups:\t",
            p->name, p->umask, st, st == 'Z' ? "zombie" : st == 'S' ? "sleeping" : "running",
            p->pid, p->pid, p->orphan ? 1 : p->ppid, p->uid, p->euid, p->suid, p->euid,
            p->gid, p->egid, p->sgid, p->egid, NFD);
        for (int g = 0; g < p->ngroups; g++) OUT("%u ", p->groups[g]);
        OUT("\n");
        OUT("VmPeak:\t%8lu kB\nVmSize:\t%8lu kB\nVmRSS:\t%8lu kB\nRssAnon:\t%8lu kB\n"
            "RssFile:\t       0 kB\nRssShmem:\t       0 kB\nVmSwap:\t       0 kB\nThreads:\t1\n",
            kb, kb, kb, kb);
        OUT("SigQ:\t0/0\nSigPnd:\t%016lx\nShdPnd:\t0000000000000000\nSigBlk:\t0000000000000000\n"
            "SigIgn:\t%016lx\nSigCgt:\t%016lx\n", p->sig_pending, p->sig_ignored, p->sig_caught);
        break;
    }
    case F_P_CMDLINE:
        n = p->cmdline_len < max ? p->cmdline_len : max;
        memcpy(gen, p->cmdline, n);
        break;
    case F_P_ENVIRON: break;
    case F_P_COMM:    OUT("%s\n", p->name); break;
    }
#undef OUT
    return n < max ? n : max - 1;
}

long procfs_read(const char *abs, uint64_t off, char *dst, size_t len)
{
    struct proc *p;
    int kind, id = lookup(abs, &p, &kind);
    if (id == F_NONE) return -ENOENT;
    if (kind == PROCFS_DIR) return -EISDIR;
    size_t n = generate(id, p);
    if (off >= n) return 0;
    if (len > n - off) len = n - off;
    memcpy(dst, gen + off, len);
    return len;
}

long procfs_readlink(const char *abs, char *dst, size_t len)
{
    struct proc *p;
    int kind, id = lookup(abs, &p, &kind);
    if (id == F_NONE) return -ENOENT;
    if (kind != PROCFS_LINK) return -EINVAL;
    char tmp[FS_PATH_MAX];
    size_t n;
    if (id == F_SELF) n = sddf_snprintf(tmp, sizeof tmp, "%d", cur->pid);
    else if (id == F_P_FDLINK) { long r = vfs_fd_describe(p, lookup_fd, tmp, sizeof tmp); n = r < 0 ? 0 : r; }
    else n = sddf_snprintf(tmp, sizeof tmp, "%s", id == F_P_EXE ? p->exe : p->cwd);
    if (n > len) n = len;
    memcpy(dst, tmp, n);
    return n;
}

/* Linux x86_64 struct stat (144 bytes) */
long procfs_stat(const char *abs, void *kstat)
{
    struct proc *p;
    int kind, id = lookup(abs, &p, &kind);
    if (id == F_NONE) return -ENOENT;
    uint8_t *st = kstat;
    memset(st, 0, 144);
    *(uint64_t *)(st + 0) = 0x50;                                   /* st_dev: "proc" */
    *(uint64_t *)(st + 8) = (uint64_t)(p ? p->pid : 0) << 8 | id;   /* st_ino */
    *(uint64_t *)(st + 16) = kind == PROCFS_DIR ? 2 : 1;            /* st_nlink */
    *(uint32_t *)(st + 24) = kind == PROCFS_DIR ? S_IFDIR | 0555
                           : kind == PROCFS_LINK ? S_IFLNK | 0777 : S_IFREG | 0444;
    if (p) {                                                        /* /proc/<pid>: its owner (ps) */
        *(uint32_t *)(st + 28) = p->euid;
        *(uint32_t *)(st + 32) = p->egid;
    }
    *(uint64_t *)(st + 56) = 1024;                                  /* st_blksize */
    uint64_t t = clock_epoch + (p ? p->start_ns : 0) / 1000000000ULL;
    *(uint64_t *)(st + 72) = t;                                     /* atime, mtime, ctime */
    *(uint64_t *)(st + 88) = t;
    *(uint64_t *)(st + 104) = t;
    return 0;
}

/* ---------------- directories ---------------- */

/* struct linux_dirent64: d_ino(8) d_off(8) d_reclen(2) d_type(1) d_name[] */
static size_t put_dirent(char *dst, size_t room, uint64_t ino, uint64_t next, int type, const char *name)
{
    size_t nl = strlen(name), reclen = (19 + nl + 1 + 7) & ~7UL;
    if (reclen > room) return 0;
    memset(dst, 0, reclen);
    *(uint64_t *)dst = ino;
    *(int64_t *)(dst + 8) = next;
    *(uint16_t *)(dst + 16) = reclen;
    dst[18] = type;
    memcpy(dst + 19, name, nl);
    return reclen;
}

/* *pos = index of the next entry */
long procfs_getdents(const char *abs, uint64_t *pos, char *dst, size_t len)
{
    struct proc *p;
    int kind, id = lookup(abs, &p, &kind);
    if (id == F_NONE) return -ENOENT;
    if (kind != PROCFS_DIR) return -ENOTDIR;
    size_t out = 0;
    for (;;) {
        uint64_t i = *pos;
        char name[16];
        const char *nm;
        int type;
        if (i < 2) {
            nm = i ? ".." : ".";
            type = DT_DIR;
        } else if (id == F_ROOT && i - 2 < N(root_ents)) {
            nm = root_ents[i - 2].name;
            type = root_ents[i - 2].dt;
        } else if (id == F_ROOT && i - 2 - N(root_ents) < NPROC) {
            struct proc *q = &procs[i - 2 - N(root_ents)];
            if (q->state == P_FREE) { (*pos)++; continue; }
            sddf_snprintf(name, sizeof name, "%d", q->pid);
            nm = name;
            type = DT_DIR;
        } else if (id == F_P_FD && i - 2 < NFD) {
            char tmp[8];
            if (vfs_fd_describe(p, i - 2, tmp, sizeof tmp) < 0) { (*pos)++; continue; }
            sddf_snprintf(name, sizeof name, "%d", (int)(i - 2));
            nm = name;
            type = DT_LNK;
        } else if (id == F_PID && i - 2 < N(pid_ents)) {
            nm = pid_ents[i - 2].name;
            type = pid_ents[i - 2].dt;
        } else if (id == F_NET && i - 2 < N(net_ents)) {
            nm = net_ents[i - 2].name;
            type = net_ents[i - 2].dt;
        } else if (id == F_SYS && i == 2) {
            nm = "kernel";
            type = DT_DIR;
        } else if (id == F_SYS_KERNEL && i - 2 < N(kernel_ents)) {
            nm = kernel_ents[i - 2].name;
            type = kernel_ents[i - 2].dt;
        } else {
            break;
        }
        size_t r = put_dirent(dst + out, len - out, i + 1, i + 1, type, nm);
        if (!r) break;
        out += r;
        (*pos)++;
    }
    return out;
}
