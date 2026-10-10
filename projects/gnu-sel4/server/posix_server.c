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
 */
#include <os/sddf.h>
#include "server.h"
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
    copy_str(u + 4 * 65, "x86_64", 65);
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

    long n = microkit_mr_get(0);
    long a1 = microkit_mr_get(1), a2 = microkit_mr_get(2), a3 = microkit_mr_get(3);
    long a4 = microkit_mr_get(4), a5 = microkit_mr_get(5), a6 = microkit_mr_get(6);

    if (proc_handles(n)) {
        r = proc_syscall(n, a1, a2, a3, a4, a5);
    } else if (vfs_handles(n)) {
        r = vfs_syscall(n, a1, a2, a3, a4, a5, a6);
    } else {
        switch (n) {
        case __NR_uname:   r = sys_uname(); break;
        case __NR_getuid: case __NR_geteuid:
        case __NR_getgid: case __NR_getegid:
                           r = 0; break;
        case __NR_setuid: case __NR_setgid:
                           r = a1 == 0 ? 0 : -EPERM; break;
        case __NR_getgroups: r = 0; break;
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
    /* A call that would block with signals pending ends with EINTR */
    if (r == POSIX_RETRY && cur->sig_pending) r = -EINTR;
    cur->blocked = r == POSIX_RETRY;          /* for /proc: state S (sleeping) */
    microkit_mr_set(0, r);
    microkit_mr_set(POSIX_MR_SIGNALS, cur->state == P_RUN ? cur->sig_pending : 0);
    cur->sig_pending = 0;
    return microkit_msginfo_new(0, POSIX_NUM_MRS);
}

/* Process faults. With VT-x, libmicrokit never replies to a fault (it assumes it
 * comes from a vCPU): if the process has to be resumed, the reply is sent right here
 * through Microkit's reply object. */
#define MICROKIT_REPLY_CAP 4

seL4_Bool fault(microkit_child child, microkit_msginfo msginfo, microkit_msginfo *reply)
{
    (void)reply;
    if (proc_fault(child, microkit_msginfo_get_label(msginfo)))
        seL4_Send(MICROKIT_REPLY_CAP, seL4_MessageInfo_new(0, 0, 0, 0));
    return seL4_False;
}

void notified(microkit_channel ch)
{
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
