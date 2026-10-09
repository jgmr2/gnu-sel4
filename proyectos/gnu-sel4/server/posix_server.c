/*
 * posix_server: implementa las llamadas al sistema POSIX que los programas
 * le envían por IPC (ver include/sel4posix/protocol.h).
 *
 *   proc.c  procesos (ranuras, exec, fork, wait, señales)
 *   tty.c   terminal (sDDF serial)
 *   vfs.c   archivos, descriptores y tuberías (servidor fs_ext2)
 *
 * Toda llamada desconocida se reporta con su nombre: es la lista de lo que
 * falta por implementar.
 */
#include <os/sddf.h>
#include "server.h"
#include "syscall_nr.h"       /* __NR_* de Linux x86_64 (generado desde musl) */
#include "syscall_names.h"    /* nombres para los mensajes (generado desde musl) */

/* Búfer compartido con el proceso que hace la llamada en curso */
uintptr_t client_buf;

static void copy_str(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    for (; src[i] && i + 1 < max; i++) dst[i] = src[i];
    for (; i < max; i++) dst[i] = 0;
}

static long sys_uname(void)
{
    /* struct utsname de Linux: 6 campos de 65 bytes */
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
    tty_puts(" no implementada -> ENOSYS\n");
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)msginfo;
    long r;
    cur = proc_by_slot((int)ch);                 /* insignia de la ranura (mem.c) */
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
        /* dormir: el runtime pasa los ns que faltan; despierta el temporizador */
        case __NR_nanosleep:
            if (a1 <= 0) { r = 0; break; }
            timer_wait(cur, a1);
            r = POSIX_RETRY;
            break;
        /* tee/splice: no hay "páginas de tubería" que mover; dash y otros
         * programas entienden EINVAL como "no aplica" y usan read/write */
        case __NR_tee: case __NR_splice: case __NR_vmsplice:
                           r = -EINVAL; break;
        default:
            report_unimplemented(n);
            r = -ENOSYS;
        }
    }
    /* Una llamada que bloquearía con señales pendientes termina con EINTR */
    if (r == POSIX_RETRY && cur->sig_pending) r = -EINTR;
    cur->blocked = r == POSIX_RETRY;          /* para /proc: estado S (durmiendo) */
    microkit_mr_set(0, r);
    microkit_mr_set(POSIX_MR_SIGNALS, cur->state == P_RUN ? cur->sig_pending : 0);
    cur->sig_pending = 0;
    return microkit_msginfo_new(0, POSIX_NUM_MRS);
}

/* Fallos de los procesos. Con VT-x, libmicrokit nunca responde a un fallo (supone
 * que es de una vCPU): si hay que reanudar el proceso, se responde aquí mismo por
 * el objeto de respuesta de Microkit. */
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
    tty_puts("[posix] servidor POSIX listo: terminal sDDF, archivos fs_ext2, hasta ");
    tty_putdec(PROC_SLOTS);
    tty_puts(" procesos con memoria a demanda\n");
    proc_start_init();
}
