/*
 * posix_server: implements the POSIX system calls that programs send it over
 * IPC (see include/sel4posix/protocol.h).
 *
 *   proc.c  processes (slots, exec, fork, wait, signals)
 *   tty.c   terminal (sDDF serial)
 *   vfs.c   files, descriptors and pipes (fs_ext2 server)
 *
 * Every unknown call is reported by name: that is the list of what is still
 * missing.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <os/sddf.h>
#include <string.h>
#include <sddf/util/printf.h>
#include "server.h"
#include "arch.h"
#include "syscall_nr.h"       /* Linux x86_64 __NR_* (generated from musl) */
#include "syscall_names.h"    /* names for the messages (generated from musl) */

/* Buffer shared with the process making the current call */
uintptr_t client_buf;

static void copy_str(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    for (; src[i] && i + 1 < max; i++) dst[i] = src[i];
    for (; i < max; i++) dst[i] = 0;
}

static long sys_uname(void)
{
    /* Linux struct utsname: 6 fields of 65 bytes */
    char *u = (char *)client_buf;
    copy_str(u + 0 * 65, "seL4", 65);
    copy_str(u + 1 * 65, "gnu-sel4", 65);
    copy_str(u + 2 * 65, "microkit-2.3.1", 65);
    copy_str(u + 3 * 65, "posix_server 0.4", 65);
    copy_str(u + 4 * 65, ARCH_UNAME, 65);
    copy_str(u + 5 * 65, "(none)", 65);
    return 0;
}

static void report_unimplemented(long n)
{
    tty_puts("[posix] ");
    tty_puts(cur->name);
    tty_puts(": syscall ");
    tty_putdec(n);
    if (n >= 0 && n < (long)(sizeof(syscall_names) / sizeof(*syscall_names)) && syscall_names[n]) {
        tty_puts(" (");
        tty_puts(syscall_names[n]);
        tty_puts(")");
    }
    tty_puts(" not implemented -> ENOSYS\n");
}

/* make TRACE=name: each call of a process called name, with its first four arguments, in a
 * ring of the last TRACE_SIZE bytes that /proc/trace shows (the console loses lines) */
#define TRACE_SIZE 0x40000
#ifdef TRACE_COMM
static char trace_ring[TRACE_SIZE];
static size_t trace_pos;               /* bytes written so far (the ring holds the last ones) */

static void trace_call(long n, long a1, long a2, long a3, long a4, long r)
{
    if (strcmp(cur->name, TRACE_COMM)) return;
    char line[256], num[16];
    const char *nm = n >= 0 && n < (long)(sizeof(syscall_names) / sizeof(*syscall_names)) ? syscall_names[n] : NULL;
    if (!nm) { sddf_snprintf(num, sizeof num, "%ld", n); nm = num; }
    int k = sddf_snprintf(line, sizeof line, "%s %d: %s(0x%lx, 0x%lx, 0x%lx, 0x%lx) = %ld\n",
                          cur->name, cur->pid, nm, a1, a2, a3, a4, r);
    if (k > (int)sizeof line - 1) k = sizeof line - 1;
    for (int i = 0; i < k; i++) trace_ring[trace_pos++ % TRACE_SIZE] = line[i];
#ifdef TRACE_CONSOLE
    tty_puts(line);                    /* make TRACE=name TRACE_CONSOLE=1: on the console too */
#endif
}
#endif

size_t trace_text(char *dst, size_t max)
{
#ifdef TRACE_COMM
    size_t have = trace_pos < TRACE_SIZE ? trace_pos : TRACE_SIZE, n = have < max ? have : max;
    size_t from = trace_pos - n;       /* the newest n bytes */
    for (size_t i = 0; i < n; i++) dst[i] = trace_ring[(from + i) % TRACE_SIZE];
    return n;
#else
    (void)dst; (void)max;
    return 0;
#endif
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)msginfo;
    long r;
    cur = proc_by_slot((int)ch);                 /* the slot's badge (mem.c) */
    if (!cur) {
        microkit_mr_set(0, -ESRCH);
        microkit_mr_set(POSIX_MR_SIGNALS, 0);
        return microkit_msginfo_new(0, POSIX_NUM_MRS);
    }
    client_buf = proc_win(cur->slot, PROC_BUF_BASE);

    /* Job control (proc.c): a stopped process stays parked; once continued, a call it had
     * finished when it stopped itself gets that result instead of running again */
    if (cur->stopped) {
        microkit_mr_set(0, POSIX_STOPPED);
        microkit_mr_set(POSIX_MR_DIRECT, 0);
        microkit_mr_set(POSIX_MR_SIGNALS, 0);
        return microkit_msginfo_new(0, POSIX_NUM_MRS);
    }
    if (cur->saved_valid) {
        cur->saved_valid = false;
        microkit_mr_set(0, cur->saved_r);
        microkit_mr_set(POSIX_MR_DIRECT, cur->saved_direct ? POSIX_DIRECT : 0);
        microkit_mr_set(POSIX_MR_SIGNALS, cur->sig_pending);
        cur->sig_pending = 0;
        return microkit_msginfo_new(0, POSIX_NUM_MRS);
    }

    uint64_t t0 = clock_rdtsc();
    reply_direct = false;
    long n = microkit_mr_get(0);
    long a1 = microkit_mr_get(1), a2 = microkit_mr_get(2), a3 = microkit_mr_get(3);
    long a4 = microkit_mr_get(4), a5 = microkit_mr_get(5), a6 = microkit_mr_get(6);

    /* Calls that fill the shared buffer with up to a3 bytes: the pages past its first 64 KiB are
     * made when one first needs them (mem.c) */
    if ((n == __NR_read || n == __NR_pread64 || n == __NR_recvfrom || n == __NR_getdents64) &&
        !mem_buf_ensure(cur->slot, (size_t)a3)) {
        microkit_mr_set(0, -ENOMEM);
        microkit_mr_set(POSIX_MR_DIRECT, 0);
        microkit_mr_set(POSIX_MR_SIGNALS, 0);
        return microkit_msginfo_new(0, POSIX_NUM_MRS);
    }

    /* A FIFO open that waited and was abandoned (a signal, then something else): drop its count */
    if (cur->fifo_wait && n != __NR_open && n != __NR_openat) fifo_cancel_wait(cur);

    if (cred_handles(n)) {
        r = cred_syscall(n, a1, a2, a3);
    } else if (proc_handles(n)) {
        r = proc_syscall(n, a1, a2, a3, a4, a5);
    } else if (vfs_handles(n)) {
        r = vfs_syscall(n, a1, a2, a3, a4, a5, a6);
    } else {
        switch (n) {
        case __NR_uname:   r = sys_uname(); break;
        /* sleep: the runtime passes the remaining ns; the timer wakes it up */
        case __NR_nanosleep:
            if (a1 <= 0) { r = 0; break; }
            timer_wait(cur, a1);
            r = POSIX_RETRY;
            break;
        /* tee/splice: there are no "pipe pages" to move; dash and other
         * programs take EINVAL as "not applicable" and use read/write */
        case __NR_tee: case __NR_splice: case __NR_vmsplice:
                           r = -EINVAL; break;
        default:
            report_unimplemented(n);
            r = -ENOSYS;
        }
    }
    if (n >= 0 && n < PERF_NSYS) perf_add(&perf_sys[n], 1, t0);
    procfs_load_tick();
    /* A call that would block with signals pending ends with EINTR */
    if (r == POSIX_RETRY && cur->sig_pending && !cur->stopped) r = -EINTR;
    if (cur->stop_self) {                     /* it stopped itself (kill, a terminal signal) */
        cur->stop_self = false;
        if (cur->stopped && cur->state == P_RUN) {
            if (r != POSIX_RETRY) {
                cur->saved_r = r;             /* given back when it calls again after SIGCONT */
                cur->saved_direct = reply_direct;
                cur->saved_valid = true;
            }
            r = POSIX_STOPPED;                /* (a call that would block simply runs again) */
        }
    }
#ifdef TRACE_COMM
    trace_call(n, a1, a2, a3, a4, r);
#endif
    cur->blocked = r == POSIX_RETRY;          /* for /proc: state S (sleeping) */
    microkit_mr_set(0, r);
    microkit_mr_set(POSIX_MR_DIRECT, reply_direct && r > 0 ? POSIX_DIRECT : 0);
    if (cur->stopped) {                       /* no handlers run while stopped: after SIGCONT */
        microkit_mr_set(POSIX_MR_SIGNALS, 0);
    } else {
        microkit_mr_set(POSIX_MR_SIGNALS, cur->state == P_RUN ? cur->sig_pending : 0);
        cur->sig_pending = 0;
    }
    return microkit_msginfo_new(0, POSIX_NUM_MRS);
}

/* Process faults. With VT-x, libmicrokit never replies to a fault (it assumes it
 * comes from a vCPU): if the process has to be resumed, the reply is sent right here
 * through Microkit's reply object. */
#ifndef __wasm__                          /* the browser port has no page faults */
#define MICROKIT_REPLY_CAP 4

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply)
{
    (void)reply;
    uint64_t t0 = clock_rdtsc();
    bool ok = proc_fault(child, microkit_msginfo_get_label(msginfo));
    perf_add(&perf_fault, 1, t0);
    if (ok) seL4_Send(MICROKIT_REPLY_CAP, seL4_MessageInfo_new(0, 0, 0, 0));
    return seL4_False;
}
#endif

void notified(microkit_channel ch)
{
    cur = NULL;           /* no process made this call: signals sent from here are never "to itself" */
    if (ch == BRIDGE_CH) bridge_notified();
    else if (ch == NET_CH) net_notified();
    else if (ch == NET_LEASE_CH) net_lease_changed();
    else if (!timer_notified(ch)) tty_notified(ch);
}

void init(void)
{
    clock_init();
    tty_init();
    bridge_init();
    vfs_init();
    proc_init();
    mem_init();
    vfs_set_fs_clock();
    tty_puts("[posix] POSIX server ready: sDDF terminal, fs_ext2 files, up to ");
    tty_putdec(PROC_SLOTS);
    tty_puts(" processes with on-demand memory\n");
    proc_start_init();
}
