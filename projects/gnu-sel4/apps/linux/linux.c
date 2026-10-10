/*
 * linux: runs a command in the Linux VM from GNU/seL4, transparently
 * (like WSL "interop").
 *
 *   linux                 interactive shell in the VM
 *   linux command args    runs "command args" in the VM
 *   docker args           (symlink /bin/docker -> linux) runs "docker args" in the VM
 *
 * If input and output are a terminal, the command runs with a pty in the VM
 * and this terminal switches to raw mode (Ctrl-C, arrows, etc. arrive as
 * they are). Otherwise it uses pipes: it works in pipelines and shell
 * scripts ("docker ps | wc -l"), and Ctrl-C is forwarded as SIGINT.
 *
 * It talks to posix_server through /dev/linux (include/sel4posix/bridge.h).
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sel4posix/bridge.h>

static int sfd = -1;
static struct termios saved;
static int raw_mode;

static void restore_tty(void)
{
    if (raw_mode) tcsetattr(0, TCSANOW, &saved);
    raw_mode = 0;
}

static int send_rec(int type, const void *data, size_t len)
{
    static char buf[1 + BR_MAX_DATA];
    if (len > BR_MAX_DATA) len = BR_MAX_DATA;
    buf[0] = type;
    if (len) memcpy(buf + 1, data, len);
    return write(sfd, buf, 1 + len) < 0 ? -1 : 0;
}

/* No terminal: Ctrl-C and friends are forwarded to the VM command */
static void forward_signal(int sig)
{
    int32_t s = sig;
    send_rec(BR_SIGNAL, &s, 4);
}

static void write_all(int fd, const char *p, size_t len)
{
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += n;
        len -= n;
    }
}

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    char *shell[] = { "/bin/sh", "-l", NULL };
    char **cmd;
    if (!strcmp(name, "linux")) cmd = argc > 1 ? argv + 1 : shell;
    else { argv[0] = (char *)name; cmd = argv; }       /* /bin/docker -> "docker ..." */

    sfd = open("/dev/linux", O_RDWR);
    if (sfd < 0) {
        fprintf(stderr, "%s: no bridge to the Linux VM (/dev/linux): %s\n", name, strerror(errno));
        return 127;
    }

    int tty = isatty(0) && isatty(1);
    struct winsize ws = { .ws_row = 24, .ws_col = 80 };
    if (tty) ioctl(1, TIOCGWINSZ, &ws);

    /* Request: struct br_open + argv */
    static char req[BR_MAX_DATA];
    struct br_open op = { .flags = tty ? BR_OPEN_TTY : 0, .rows = ws.ws_row, .cols = ws.ws_col };
    size_t len = sizeof op;
    for (char **a = cmd; *a; a++) {
        size_t l = strlen(*a) + 1;
        if (len + l > sizeof req) { fprintf(stderr, "%s: arguments too long\n", name); return 126; }
        memcpy(req + len, *a, l);
        len += l;
        op.argc++;
    }
    memcpy(req, &op, sizeof op);
    if (send_rec(BR_OPEN, req, len)) { perror(name); return 126; }

    if (tty) {
        tcgetattr(0, &saved);
        struct termios raw = saved;
        raw.c_iflag &= ~(ICRNL | IXON | INLCR | IGNCR);
        raw.c_oflag &= ~OPOST;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
        raw_mode = 1;
        atexit(restore_tty);
    } else {
        struct sigaction sa = { .sa_handler = forward_signal };
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
    }

    static char buf[1 + BR_MAX_DATA + 16];
    int stdin_open = 1;
    for (;;) {
        struct pollfd pf[2] = { { .fd = sfd, .events = POLLIN }, { .fd = 0, .events = POLLIN } };
        if (poll(pf, stdin_open ? 2 : 1, -1) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (pf[0].revents) {
            ssize_t n = read(sfd, buf, sizeof buf);
            if (n <= 0) {                                   /* the VM restarted */
                restore_tty();
                fprintf(stderr, "%s: lost the connection to the VM\n", name);
                return 255;
            }
            switch (buf[0]) {
            case BR_STDOUT: write_all(1, buf + 1, n - 1); break;
            case BR_STDERR: write_all(2, buf + 1, n - 1); break;
            case BR_EXIT: {
                int32_t code = 0;
                if (n >= 5) memcpy(&code, buf + 1, 4);
                restore_tty();
                return code;
            }
            }
        }
        if (stdin_open && (pf[1].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(0, buf, BR_MAX_DATA);
            if (n > 0) send_rec(BR_STDIN, buf, n);
            else if (n == 0 || (n < 0 && errno != EINTR)) {
                send_rec(BR_STDIN_EOF, NULL, 0);
                stdin_open = 0;
            }
        }
    }
    restore_tty();
    return 255;
}
