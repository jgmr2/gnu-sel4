/*
 * posix_server's terminal: a client of the sDDF serial subsystem (queues in
 * memory shared with serial_virt_rx/tx) with a "line discipline" like the
 * Linux TTY's: echo, line editing, Ctrl-D and raw mode via termios.
 */
#include <os/sddf.h>
#include <sddf/serial/queue.h>
#include <sddf/serial/config.h>
#include "server.h"

/* terminal ioctls (Linux) */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCNOTTY  0x5422

/* termios bits the line discipline interprets */
#define ICRNL   0000400     /* c_iflag: \r -> \n */
#define OPOST   0000001     /* c_oflag */
#define ONLCR   0000004     /* c_oflag: \n -> \r\n */
#define ISIG    0000001     /* c_lflag */
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define IEXTEN  0100000
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6

/* Linux/musl struct termios: 60 bytes */
struct k_termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t c_ispeed, c_ospeed;
} __attribute__((packed, aligned(4)));

/* Serial client configuration (written by the build with objcopy) */
__attribute__((__section__(".serial_client_config"))) serial_client_config_t serial_config;
static serial_queue_handle_t rx_queue, tx_queue;

static struct k_termios tio = {
    .c_iflag = ICRNL,
    .c_oflag = OPOST | ONLCR,
    .c_cflag = 0000277,   /* B38400 | CS8 | CREAD */
    .c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN,
    .c_cc = { [VINTR] = 3, [VQUIT] = 0x1c, [VERASE] = 0x7f, [VKILL] = 0x15, [VEOF] = 4, [VTIME] = 0, [VMIN] = 1 },
    .c_ispeed = 38400, .c_ospeed = 38400,
};

#define LINE_MAX_ 4096
static char line[LINE_MAX_];
static uint32_t line_len;    /* bytes in the input buffer */
static uint32_t ready_len;   /* of those, ready for read() (finished line or raw mode) */
static bool eof_pending;
static uint32_t reader_waiting, writer_waiting;   /* waiting processes (bit = index) */
static int fg_pgrp = 1;                            /* foreground group (receives Ctrl-C) */
/* Terminal size: it does not arrive over the serial port; `resize` sets it (TIOCSWINSZ)
 * after asking the terminal with an escape sequence */
static unsigned short win_rows = 24, win_cols = 80;

void tty_set_fg(int pgid) { fg_pgrp = pgid; }
int tty_get_fg(void) { return fg_pgrp; }

static uint32_t tx_tail;     /* local transmit tail (published in tty_flush) */

static bool tx_put(char c)
{
    if (serial_queue_full(&tx_queue, tx_tail)) return false;
    serial_enqueue_local(&tx_queue, &tx_tail, c);
    return true;
}

static void tty_flush(void)
{
    serial_update_shared_tail(&tx_queue, tx_tail);
    sddf_notify(serial_config.tx.id);
}

/* Writes a byte applying output post-processing (\n -> \r\n). */
static bool tty_out(char c)
{
    if ((tio.c_oflag & OPOST) && (tio.c_oflag & ONLCR) && c == '\n') {
        if (serial_queue_full(&tx_queue, tx_tail) || serial_queue_full(&tx_queue, tx_tail + 1)) return false;
        tx_put('\r');
    }
    return tx_put(c);
}

/* The server's own messages: \n -> \r\n, as ONLCR does with program output */
void tty_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') tty_out('\r');
        tty_out(*s);
    }
    tty_flush();
}

void tty_putdec(long v)
{
    char tmp[24];
    int i = 0;
    if (v < 0) { tty_out('-'); v = -v; }
    do { tmp[i++] = '0' + v % 10; v /= 10; } while (v);
    while (i) tty_out(tmp[--i]);
    tty_flush();
}

static void echo_char(char c)
{
    if (!(tio.c_lflag & ECHO)) return;
    if ((unsigned char)c < 0x20 && c != '\n' && c != '\t') {   /* control: ^X */
        tty_out('^');
        tty_out(c + 0x40);
    } else {
        tty_out(c);
    }
}

static void erase_one(void)
{
    if (line_len <= ready_len) return;     /* only the current line can be edited */
    line_len--;
    if ((tio.c_lflag & ECHO) && (tio.c_lflag & ECHOE)) {
        tty_out('\b'); tty_out(' '); tty_out('\b');
    }
}

/* Processes a character received from the keyboard */
static void tty_input(char c)
{
    if ((tio.c_iflag & ICRNL) && c == '\r') c = '\n';

    if (tio.c_lflag & ICANON) {
        if (c == tio.c_cc[VERASE] || c == '\b') { erase_one(); return; }
        if (c == tio.c_cc[VKILL]) { while (line_len > ready_len) erase_one(); return; }
        if (c == tio.c_cc[VEOF]) {
            if (line_len == ready_len) eof_pending = true;   /* Ctrl-D on an empty line: end of file */
            else ready_len = line_len;                       /* otherwise, hand over what was typed */
            return;
        }
        if ((tio.c_lflag & ISIG) && (c == tio.c_cc[VINTR] || c == tio.c_cc[VQUIT])) {
            /* Ctrl-C / Ctrl-\: discard the line and signal the foreground group */
            line_len = ready_len;
            tty_puts(c == tio.c_cc[VINTR] ? "^C\n" : "^\\\n");
            proc_signal_pgrp(fg_pgrp, c == tio.c_cc[VINTR] ? SIGINT : SIGQUIT);
            return;
        }
        if (line_len < LINE_MAX_) line[line_len++] = c;
        echo_char(c);
        if (c == '\n') ready_len = line_len;
    } else {
        if (line_len < LINE_MAX_) line[line_len++] = c;
        echo_char(c);
        ready_len = line_len;
    }
}

long tty_read(size_t len)
{
    if (ready_len == 0) {
        if (eof_pending) { eof_pending = false; return 0; }
        reader_waiting |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    size_t n = len < ready_len ? len : ready_len;
    if (n > POSIX_BUF_SIZE) n = POSIX_BUF_SIZE;
    char *out = (char *)client_buf;
    for (size_t i = 0; i < n; i++) out[i] = line[i];
    for (size_t i = n; i < line_len; i++) line[i - n] = line[i];
    line_len -= n;
    ready_len -= n;
    return n;
}

long tty_write(const char *data, size_t len)
{
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    size_t n = 0;
    while (n < len && tty_out(data[n])) n++;
    if (n) {
        tty_flush();
        return n;
    }
    /* Queue full: ask serial_virt_tx for a signal when there is room, and check again */
    serial_request_consumer_signal(&tx_queue);
    if (!serial_queue_full(&tx_queue, tx_tail + 1)) return tty_write(data, len);
    writer_waiting |= 1u << PROC_INDEX(cur);
    return POSIX_RETRY;
}

static void copy_bytes(void *dst, const void *src, size_t n)
{
    for (size_t i = 0; i < n; i++) ((char *)dst)[i] = ((const char *)src)[i];
}

long tty_ioctl(long req, long arg)
{
    switch (req) {
    case TIOCGPGRP:
        microkit_mr_set(1, fg_pgrp);
        return 0;
    case TIOCSPGRP:
        fg_pgrp = arg;
        return 0;
    case TIOCSCTTY:
    case TIOCNOTTY:
        return 0;
    case TIOCGWINSZ: {
        unsigned short *ws = (unsigned short *)client_buf;   /* struct winsize */
        ws[0] = win_rows; ws[1] = win_cols; ws[2] = 0; ws[3] = 0;
        return 0;
    }
    case TIOCSWINSZ: {
        const unsigned short *ws = (const unsigned short *)client_buf;
        if (!ws[0] || !ws[1]) return -EINVAL;
        bool changed = ws[0] != win_rows || ws[1] != win_cols;
        win_rows = ws[0];
        win_cols = ws[1];
        if (changed) proc_signal_pgrp(fg_pgrp, SIGWINCH);
        return 0;
    }
    case TCGETS:
        copy_bytes((void *)client_buf, &tio, sizeof(tio));
        return 0;
    case TCSETSF:
        line_len = ready_len = 0;                              /* discard pending input */
        /* fallthrough */
    case TCSETS:
    case TCSETSW:
        copy_bytes(&tio, (void *)client_buf, sizeof(tio));
        if (!(tio.c_lflag & ICANON)) ready_len = line_len;     /* in raw mode everything is ready */
        return 0;
    }
    return -ENOTTY;
}

void tty_notified(unsigned ch)
{
    if (ch == serial_config.rx.id) {
        char c;
        while (!serial_dequeue(&rx_queue, &c)) tty_input(c);
        tty_flush();
        if (reader_waiting && (ready_len || eof_pending)) {
            proc_wake_mask(reader_waiting);
            reader_waiting = 0;
        }
    } else if (ch == serial_config.tx.id) {
        if (writer_waiting) {
            proc_wake_mask(writer_waiting);
            writer_waiting = 0;
        }
    }
}

void tty_init(void)
{
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

/* poll(): 1 = POLLIN, 4 = POLLOUT; with wait, it registers to be woken up */
int tty_poll(bool want_in, bool want_out, bool wait)
{
    int ready = 0;
    if (want_in && (ready_len || eof_pending)) ready |= 1;
    if (want_out && !serial_queue_full(&tx_queue, tx_tail + 1)) ready |= 4;
    if (!ready && wait) {
        if (want_in) reader_waiting |= 1u << PROC_INDEX(cur);
        if (want_out) {
            serial_request_consumer_signal(&tx_queue);
            writer_waiting |= 1u << PROC_INDEX(cur);
        }
    }
    return ready;
}
