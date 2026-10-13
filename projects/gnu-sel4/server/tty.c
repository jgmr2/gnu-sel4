/*
 * posix_server's terminals, each with a "line discipline" like the Linux TTY's: echo, line
 * editing, Ctrl-D, the signal characters and raw mode via termios.
 *   tty 0      the console: a client of the sDDF serial subsystem (queues in memory shared with
 *              serial_virt_rx/tx); /dev/console
 *   tty 1..N   pseudo-terminals: /dev/ptmx gives the master side (what a terminal emulator, sshd
 *              or tmux holds), /dev/pts/<n> the slave side (what the shell runs on). What the
 *              master writes is the slave's keyboard; what the slave writes (after output
 *              processing, and the echo) is read from the master
 * Sessions: a terminal is the controlling terminal of at most one session, which gets it when its
 * leader opens it (or TIOCSCTTY); /dev/tty is the caller's. Its foreground process group gets
 * the signal characters. A hangup (the master closed, the session leader exited) sends SIGHUP.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <os/sddf.h>
#include <sddf/serial/queue.h>
#include <sddf/serial/config.h>
#include <sddf/util/printf.h>
#include <string.h>
#include "server.h"

/* terminal ioctls (Linux) */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TCSBRK     0x5409
#define TCXONC     0x540A
#define TCFLSH     0x540B
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCOUTQ   0x5411
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD_  0x541B
#define TIOCPKT    0x5420
#define TIOCNOTTY  0x5422
#define TIOCGSID   0x5429
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431

/* termios bits the line discipline interprets */
#define ICRNL   0000400     /* c_iflag: \r -> \n */
#define OPOST   0000001     /* c_oflag */
#define ONLCR   0000004     /* c_oflag: \n -> \r\n */
#define ISIG    0000001     /* c_lflag */
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define NOFLSH  0000200
#define TOSTOP  0000400
#define IEXTEN  0100000
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSUSP 10

/* Linux/musl struct termios: 60 bytes */
struct k_termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t c_ispeed, c_ospeed;
} __attribute__((packed, aligned(4)));

static const struct k_termios tio_default = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = 0000277,   /* B38400 | CS8 | CREAD */
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_cc = { [VINTR] = 3, [VQUIT] = 0x1c, [VERASE] = 0x7f, [VKILL] = 0x15, [VEOF] = 4, [VTIME] = 0,
              [VMIN] = 1, [VSUSP] = 0x1a },
    .c_ispeed = 38400, .c_ospeed = 38400,
};

#define LINE_MAX_ 4096
#define PTY_BUF   8192              /* slave output waiting for the master */

struct tty {
    bool used;
    bool pty, locked;               /* pty: unlockpt() not called yet */
    bool master_open, hup;          /* hup: the master is gone (slave: EOF on read, EIO on write) */
    int slave_opens;                /* open file descriptions of the slave */
    struct k_termios tio;
    char line[LINE_MAX_];           /* input */
    uint32_t line_len;              /* bytes in the input buffer */
    uint32_t ready_len;             /* of those, ready for read() (finished line or raw mode) */
    bool eof_pending;
    waitmask_t readers, writers;    /* processes waiting on the slave (or the console) */
    unsigned short rows, cols;
    int sid;                        /* the session it controls (0: none) */
    int fg_pgrp;                    /* its foreground process group (gets Ctrl-C, Ctrl-Z...) */
    /* pty: the master's side */
    char out[PTY_BUF];
    uint32_t out_head, out_len;
    waitmask_t m_readers, m_writers;
    /* Packet mode (TIOCPKT, which screen uses): each read of the master starts with a status
     * byte, 0 before data; a flush of the slave is told alone (TIOCPKT_FLUSHREAD/WRITE) */
    bool pkt;
    uint8_t pkt_status;
};
static struct tty ttys[NTTY];

/* Serial client configuration (written by the build with objcopy) */
__attribute__((__section__(".serial_client_config"))) serial_client_config_t serial_config;
static serial_queue_handle_t rx_queue, tx_queue;
static uint32_t tx_tail;     /* local transmit tail (published in console_flush) */

static struct tty *tty_at(int i) { return i >= 0 && i < NTTY && ttys[i].used ? &ttys[i] : NULL; }
bool tty_exists(int i) { return tty_at(i) != NULL; }

static void console_flush(void)
{
    serial_update_shared_tail(&tx_queue, tx_tail);
    sddf_notify(serial_config.tx.id);
}

/* Room for n more output bytes? */
static bool out_room(struct tty *t, uint32_t n)
{
    if (t->pty) return t->out_len + n <= PTY_BUF;
    return !serial_queue_full(&tx_queue, tx_tail + n - 1);
}

static void out_raw(struct tty *t, char c)
{
    if (t->pty) {
        t->out[(t->out_head + t->out_len) % PTY_BUF] = c;
        t->out_len++;
    } else {
        serial_enqueue_local(&tx_queue, &tx_tail, c);
    }
}

/* Writes a byte applying output post-processing (\n -> \r\n) */
static bool tty_out(struct tty *t, char c)
{
    bool crlf = (t->tio.c_oflag & OPOST) && (t->tio.c_oflag & ONLCR) && c == '\n';
    if (!out_room(t, crlf ? 2 : 1)) return false;
    if (crlf) out_raw(t, '\r');
    out_raw(t, c);
    return true;
}

/* Output is there: the console is told, a pty's master readers woken up */
static void out_done(struct tty *t)
{
    if (!t->pty) {
        console_flush();
    } else if (t->m_readers && t->out_len) {
        proc_wake_mask(t->m_readers);
        t->m_readers = 0;
    }
}

/* The server's own messages, on the console: \n -> \r\n, as ONLCR does with program output */
void tty_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n' && !serial_queue_full(&tx_queue, tx_tail)) serial_enqueue_local(&tx_queue, &tx_tail, '\r');
        if (!serial_queue_full(&tx_queue, tx_tail)) serial_enqueue_local(&tx_queue, &tx_tail, *s);
    }
    console_flush();
}

void tty_putdec(long v)
{
    char tmp[24];
    int i = 0;
    if (v < 0) { tty_puts("-"); v = -v; }
    do { tmp[i++] = '0' + v % 10; v /= 10; } while (v);
    tmp[i] = 0;
    for (int a = 0, b = i - 1; a < b; a++, b--) { char x = tmp[a]; tmp[a] = tmp[b]; tmp[b] = x; }
    tty_puts(tmp);
}

static void echo_char(struct tty *t, char c)
{
    if (!(t->tio.c_lflag & ECHO)) return;
    if ((unsigned char)c < 0x20 && c != '\n' && c != '\t') {   /* control: ^X */
        tty_out(t, '^');
        tty_out(t, c + 0x40);
    } else {
        tty_out(t, c);
    }
}

static void erase_one(struct tty *t)
{
    if (t->line_len <= t->ready_len) return;     /* only the current line can be edited */
    t->line_len--;
    if ((t->tio.c_lflag & ECHO) && (t->tio.c_lflag & ECHOE)) {
        tty_out(t, '\b'); tty_out(t, ' '); tty_out(t, '\b');
    }
}

static void wake_readers(struct tty *t)
{
    if (t->readers && (t->ready_len || t->eof_pending || t->hup)) {
        proc_wake_mask(t->readers);
        t->readers = 0;
    }
}

/* A character from the keyboard (the console's, or what the master wrote). false: no room */
static bool tty_input(struct tty *t, char c)
{
    if ((t->tio.c_iflag & ICRNL) && c == '\r') c = '\n';
    uint8_t *cc = t->tio.c_cc;
    if ((t->tio.c_lflag & ISIG) && c && (c == cc[VINTR] || c == cc[VQUIT] || c == cc[VSUSP])) {
        /* Ctrl-C, Ctrl-\, Ctrl-Z: discard the pending input and signal the foreground group */
        if (!(t->tio.c_lflag & NOFLSH)) t->line_len = t->ready_len = 0;
        if (t->tio.c_lflag & ECHO) {            /* "^C", as Linux (the shell starts the new line) */
            tty_out(t, '^');
            tty_out(t, c + 0x40);
        }
        if (t->fg_pgrp > 0)
            proc_signal_pgrp(t->fg_pgrp, c == cc[VINTR] ? SIGINT : c == cc[VQUIT] ? SIGQUIT : SIGTSTP);
        return true;
    }
    if (t->tio.c_lflag & ICANON) {
        if (c == cc[VERASE] || c == '\b') { erase_one(t); return true; }
        if (c == cc[VKILL]) { while (t->line_len > t->ready_len) erase_one(t); return true; }
        if (c == cc[VEOF]) {
            if (t->line_len == t->ready_len) t->eof_pending = true;   /* Ctrl-D on an empty line: end of file */
            else t->ready_len = t->line_len;                          /* otherwise, hand over what was typed */
            return true;
        }
        if (t->line_len < LINE_MAX_) t->line[t->line_len++] = c;      /* a full line drops the rest */
        echo_char(t, c);
        if (c == '\n') t->ready_len = t->line_len;
    } else {
        if (t->line_len >= LINE_MAX_) return false;
        t->line[t->line_len++] = c;
        echo_char(t, c);
        t->ready_len = t->line_len;
    }
    return true;
}

/* ---------------- sessions ---------------- */

int tty_ctty(const struct proc *p)
{
    for (int i = 0; i < NTTY; i++)
        if (ttys[i].used && ttys[i].sid && ttys[i].sid == p->sid) return i;
    return -1;
}

int tty_fg_of(int i) { struct tty *t = tty_at(i); return t ? t->fg_pgrp : 0; }

/* The terminal is no longer the session's: SIGHUP (and SIGCONT, to whatever is stopped) to its
 * foreground group, and to the session leader on a hangup */
static void hangup(struct tty *t, bool leader_too)
{
    int sid = t->sid, fg = t->fg_pgrp;
    t->sid = 0;
    t->fg_pgrp = 0;
    if (leader_too && sid > 0) proc_signal(proc_by_pid(sid), SIGHUP);
    if (fg > 0) {
        proc_signal_pgrp(fg, SIGHUP);
        proc_signal_pgrp(fg, SIGCONT);
    }
}

void tty_console_session(int sid)
{
    ttys[0].sid = sid;
    ttys[0].fg_pgrp = sid;
}

void tty_session_leader_exit(const struct proc *p)
{
    if (p->pid != p->sid) return;
    int i = tty_ctty(p);
    if (i >= 0) hangup(&ttys[i], false);
}

/* Opening a terminal: a session leader without one takes it (unless O_NOCTTY) */
static void maybe_take(struct tty *t, uint32_t flags)
{
    if ((flags & 0400 /* O_NOCTTY */) || t->sid || cur->pid != cur->sid || tty_ctty(cur) >= 0) return;
    t->sid = cur->sid;
    t->fg_pgrp = cur->pgid;
}

/* ---------------- opening and closing ---------------- */

/* /dev/console, /dev/tty, /dev/ptmx, /dev/pts/<n>: tty index (master: *master = true) or -errno */
long tty_open(const char *abs, uint32_t flags, bool *master)
{
    *master = false;
    if (!strcmp(abs, "/dev/console")) {
        maybe_take(&ttys[0], flags);
        return 0;
    }
    if (!strcmp(abs, "/dev/tty")) {
        int i = tty_ctty(cur);
        if (i < 0) return -ENXIO;
        if (ttys[i].pty) ttys[i].slave_opens++;
        return i;
    }
    if (!strcmp(abs, "/dev/ptmx")) {
        for (int i = 1; i < NTTY; i++) {
            struct tty *t = &ttys[i];
            if (t->used) continue;
            *t = (struct tty){ .used = true, .pty = true, .locked = true, .master_open = true,
                               .tio = tio_default, .rows = 24, .cols = 80 };
            *master = true;
            return i;
        }
        return -ENOSPC;
    }
    int n = tty_pts_number(abs);
    if (n >= 0) {
        struct tty *t = tty_at(n + 1);
        if (!t || !t->pty) return -ENOENT;
        if (t->locked || t->hup) return -EIO;
        t->slave_opens++;
        maybe_take(t, flags);
        return n + 1;
    }
    return -ENOENT;
}

/* "/dev/pts/<n>" -> n, or -1 */
int tty_pts_number(const char *abs)
{
    if (strncmp(abs, "/dev/pts/", 9) || !abs[9]) return -1;
    int n = 0;
    for (const char *s = abs + 9; *s; s++) {
        if (*s < '0' || *s > '9' || n > 1000) return -1;
        n = n * 10 + (*s - '0');
    }
    return n < NTTY - 1 ? n : -1;
}

static void pty_maybe_free(struct tty *t)
{
    if (t->pty && !t->master_open && t->slave_opens <= 0) t->used = false;
}

void tty_close(int i, bool master)
{
    struct tty *t = tty_at(i);
    if (!t || !t->pty) return;
    if (master) {
        t->master_open = false;
        t->hup = true;
        if (t->sid) hangup(t, true);
        proc_wake_mask(t->readers | t->writers);
        t->readers = t->writers = 0;
    } else if (--t->slave_opens <= 0) {
        if (t->m_readers) { proc_wake_mask(t->m_readers); t->m_readers = 0; }   /* EIO for them */
    }
    pty_maybe_free(t);
}

/* ---------------- the slave side (and the console) ---------------- */

/* A background process using its controlling terminal: SIGTTIN (reading) or SIGTTOU (writing,
 * with TOSTOP) to its group, which stops it by default; the call is retried once it is
 * continued (or ends with EINTR if it catches the signal). Ignored, or nobody to continue it
 * (an orphaned group): EIO. 0: it may go on */
static long background_check(struct tty *t, int sig)
{
    if (!t->sid || t->sid != cur->sid || cur->pgid == t->fg_pgrp || t->fg_pgrp <= 0) return 0;
    if ((cur->sig_ignored & SIGBIT(sig)) || proc_pgrp_orphaned(cur->pgid)) return sig == SIGTTIN ? -EIO : 0;
    proc_signal_pgrp(cur->pgid, sig);
    return POSIX_RETRY;
}

long tty_read(int i, size_t len)
{
    struct tty *t = tty_at(i);
    if (!t) return -EIO;
    long bg = background_check(t, SIGTTIN);
    if (bg) return bg;
    if (t->ready_len == 0) {
        if (t->eof_pending) { t->eof_pending = false; return 0; }
        if (t->hup) return 0;
        t->readers |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    size_t n = len < t->ready_len ? len : t->ready_len;
    if (n > POSIX_BUF_SIZE) n = POSIX_BUF_SIZE;
    if (t->tio.c_lflag & ICANON)                 /* canonical: one line per read(), as POSIX says */
        for (size_t k = 0; k < n; k++) if (t->line[k] == '\n') { n = k + 1; break; }
    char *out = (char *)client_buf;
    memcpy(out, t->line, n);
    memmove(t->line, t->line + n, t->line_len - n);
    t->line_len -= n;
    t->ready_len -= n;
    if (t->pty && t->m_writers) { proc_wake_mask(t->m_writers); t->m_writers = 0; }   /* room */
    return n;
}

long tty_write(int i, const char *data, size_t len)
{
    struct tty *t = tty_at(i);
    if (!t) return -EIO;
    if (t->hup) return -EIO;
    if (t->tio.c_lflag & TOSTOP) {
        long bg = background_check(t, SIGTTOU);
        if (bg) return bg;
    }
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    size_t n = 0;
    while (n < len && tty_out(t, data[n])) n++;
    if (n) {
        out_done(t);
        return n;
    }
    if (!t->pty) {
        /* Queue full: ask serial_virt_tx for a signal when there is room, and check again */
        serial_request_consumer_signal(&tx_queue);
        if (!serial_queue_full(&tx_queue, tx_tail + 1)) return tty_write(i, data, len);
    }
    t->writers |= WAIT_BIT(cur);
    return POSIX_RETRY;
}

/* ---------------- the master side ---------------- */

long pty_master_read(int i, size_t len)
{
    struct tty *t = tty_at(i);
    if (!t) return -EIO;
    char *out = (char *)client_buf;
    if (t->pkt && t->pkt_status) {                       /* a status packet: one byte */
        if (!len) return 0;
        out[0] = t->pkt_status;
        t->pkt_status = 0;
        return 1;
    }
    if (!t->out_len) {
        if (t->slave_opens <= 0 && !t->locked) return -EIO;   /* as Linux: the slave was closed */
        t->m_readers |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    if (t->pkt) {                                        /* TIOCPKT_DATA, then the data */
        if (len < 2) return -EINVAL;
        out[0] = 0;
        out++;
        len--;
    }
    size_t n = len < t->out_len ? len : t->out_len;
    if (n > POSIX_BUF_SIZE - 1) n = POSIX_BUF_SIZE - 1;
    for (size_t k = 0; k < n; k++) out[k] = t->out[(t->out_head + k) % PTY_BUF];
    t->out_head = (t->out_head + n) % PTY_BUF;
    t->out_len -= n;
    if (t->writers) { proc_wake_mask(t->writers); t->writers = 0; }   /* the slave may write again */
    return n + t->pkt;
}

long pty_master_write(int i, const char *data, size_t len)
{
    struct tty *t = tty_at(i);
    if (!t) return -EIO;
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    size_t n = 0;
    while (n < len && out_room(t, 3) && tty_input(t, data[n])) n++;   /* room for the echo */
    out_done(t);
    wake_readers(t);
    if (n) return n;
    t->m_writers |= WAIT_BIT(cur);
    return POSIX_RETRY;
}

int pty_master_poll(int i, bool want_in, bool want_out, bool wait)
{
    struct tty *t = tty_at(i);
    if (!t) return 8;                                       /* POLLERR */
    int ready = 0;
    if (want_in && (t->out_len || (t->pkt && t->pkt_status) || (t->slave_opens <= 0 && !t->locked))) ready |= 1;
    if (want_out && t->line_len < LINE_MAX_ && out_room(t, 3)) ready |= 4;
    if (!t->out_len && t->slave_opens <= 0 && !t->locked) ready |= 16;   /* POLLHUP */
    if (!ready && wait) {
        if (want_in) t->m_readers |= WAIT_BIT(cur);
        if (want_out) t->m_writers |= WAIT_BIT(cur);
    }
    return ready;
}

/* ---------------- ioctl ---------------- */

static void copy_bytes(void *dst, const void *src, size_t n)
{
    for (size_t k = 0; k < n; k++) ((char *)dst)[k] = ((const char *)src)[k];
}

long tty_ioctl(int i, bool master, long req, long arg)
{
    struct tty *t = tty_at(i);
    if (!t) return -EIO;
    switch ((uint32_t)req) {           /* ioctl()'s request is an int: 0x80045430 arrives negative */
    case TIOCGPTN:
        if (!t->pty) return -ENOTTY;
        microkit_mr_set(1, i - 1);
        return 0;
    case TIOCSPTLCK:
        if (!master) return -ENOTTY;
        t->locked = arg != 0;
        return 0;
    case TIOCPKT:
        if (!master) return -ENOTTY;
        t->pkt = arg != 0;
        t->pkt_status = 0;
        return 0;
    case TIOCGPGRP:
        microkit_mr_set(1, t->fg_pgrp);
        return 0;
    case TIOCGSID:
        if (!t->sid) return -ENOTTY;
        microkit_mr_set(1, t->sid);
        return 0;
    case TIOCSPGRP:
        if (!master && t->sid && t->sid != cur->sid) return -ENOTTY;   /* not its controlling terminal */
        t->fg_pgrp = arg;
        return 0;
    case TIOCSCTTY:
        if (cur->pid != cur->sid) return -EPERM;
        if (t->sid == cur->sid) return 0;
        if (t->sid && !(arg == 1 && cur->euid == 0)) return -EPERM;   /* someone else's; root may steal it */
        if (tty_ctty(cur) >= 0) return -EPERM;
        t->sid = cur->sid;
        t->fg_pgrp = cur->pgid;
        return 0;
    case TIOCNOTTY:
        if (t->sid != cur->sid) return -ENOTTY;
        if (cur->pid == cur->sid) hangup(t, false);
        return 0;
    case TIOCGWINSZ: {
        unsigned short *ws = (unsigned short *)client_buf;   /* struct winsize */
        ws[0] = t->rows; ws[1] = t->cols; ws[2] = 0; ws[3] = 0;
        return 0;
    }
    case TIOCSWINSZ: {
        const unsigned short *ws = (const unsigned short *)client_buf;
        if (!ws[0] || !ws[1]) return -EINVAL;
        bool changed = ws[0] != t->rows || ws[1] != t->cols;
        t->rows = ws[0];
        t->cols = ws[1];
        if (changed && t->fg_pgrp > 0) proc_signal_pgrp(t->fg_pgrp, SIGWINCH);
        return 0;
    }
    case FIONREAD_:
        microkit_mr_set(1, master ? t->out_len : t->ready_len);
        return 0;
    case TIOCOUTQ:
        microkit_mr_set(1, t->pty ? t->out_len : 0);
        return 0;
    case TCSBRK:                       /* tcdrain(): the output is already handed over */
    case TCXONC:
        return 0;
    case TCFLSH:                       /* tcflush(): 0 = input, 1 = output, 2 = both */
        if (arg == 0 || arg == 2) t->line_len = t->ready_len = 0;
        if ((arg == 1 || arg == 2) && t->pty) t->out_len = 0;
        if (t->pkt && !master && arg >= 0 && arg <= 2) {    /* told to the master's reader */
            t->pkt_status |= arg == 0 ? 1 : arg == 1 ? 2 : 3;     /* TIOCPKT_FLUSHREAD, _FLUSHWRITE */
            if (t->m_readers) { proc_wake_mask(t->m_readers); t->m_readers = 0; }
        }
        return 0;
    case TCGETS:
        copy_bytes((void *)client_buf, &t->tio, sizeof(t->tio));
        return 0;
    case TCSETSF:
        t->line_len = t->ready_len = 0;                              /* discard pending input */
        /* fallthrough */
    case TCSETS:
    case TCSETSW:
        copy_bytes(&t->tio, (void *)client_buf, sizeof(t->tio));
        if (!(t->tio.c_lflag & ICANON)) t->ready_len = t->line_len;  /* in raw mode everything is ready */
        wake_readers(t);
        return 0;
    }
    return -ENOTTY;
}

/* ---------------- console events, poll ---------------- */

void tty_notified(unsigned ch)
{
    struct tty *t = &ttys[0];
    if (ch == serial_config.rx.id) {
        char c;
        while (!serial_dequeue(&rx_queue, &c)) tty_input(t, c);
        console_flush();
        wake_readers(t);
    } else if (ch == serial_config.tx.id) {
        if (t->writers) {
            proc_wake_mask(t->writers);
            t->writers = 0;
        }
    }
}

void tty_init(void)
{
    ttys[0] = (struct tty){ .used = true, .tio = tio_default, .rows = 24, .cols = 80 };
    if (!serial_config_check_magic(&serial_config)) {
        microkit_dbg_puts("[posix] invalid serial configuration\n");
        return;
    }
    serial_queue_init(&rx_queue, serial_config.rx.queue.vaddr, serial_config.rx.data.size,
                      serial_config.rx.data.vaddr);
    serial_queue_init(&tx_queue, serial_config.tx.queue.vaddr, serial_config.tx.data.size,
                      serial_config.tx.data.vaddr);
    tx_tail = tx_queue.queue->tail;
}

/* poll(): 1 = POLLIN, 4 = POLLOUT, 16 = POLLHUP; with wait, it registers to be woken up */
int tty_poll(int i, bool want_in, bool want_out, bool wait)
{
    struct tty *t = tty_at(i);
    if (!t) return 8;                                       /* POLLERR */
    int ready = 0;
    if (t->hup) return 16 | (want_in ? 1 : 0);
    if (want_in && (t->ready_len || t->eof_pending)) ready |= 1;
    if (want_out && out_room(t, 2)) ready |= 4;
    if (!ready && wait) {
        if (want_in) t->readers |= WAIT_BIT(cur);
        if (want_out) {
            if (!t->pty) serial_request_consumer_signal(&tx_queue);
            t->writers |= WAIT_BIT(cur);
        }
    }
    return ready;
}

/* For stat() and /proc: the device number (st_rdev) and the name */
uint32_t tty_rdev(int i) { return i == 0 ? 0x0501 : 0x8800 + (i - 1); }   /* (5,1), (136,n) */
void tty_name(int i, char *out, size_t max)
{
    if (i == 0) sddf_snprintf(out, max, "/dev/console");
    else sddf_snprintf(out, max, "/dev/pts/%d", i - 1);
}
