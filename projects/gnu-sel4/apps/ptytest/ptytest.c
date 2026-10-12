/*
 * ptytest: pseudo-terminals and sessions (server/tty.c), each check on its own line.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
                               else { printf("  FAIL %s (errno %d: %s)\n", what, errno, strerror(errno)); failures++; } } while (0)

/* Reads from fd until it has want bytes or nothing comes for a while */
static int read_some(int fd, char *buf, int max, int ms)
{
    int n = 0;
    struct pollfd p = { .fd = fd, .events = POLLIN };
    while (n < max && poll(&p, 1, ms) > 0) {
        int r = read(fd, buf + n, max - n);
        if (r <= 0) break;
        n += r;
    }
    buf[n] = 0;
    return n;
}

int main(void)
{
    printf("== pseudo-terminals ==\n");
    int m, s;
    char name[64];
    CHECK(openpty(&m, &s, name, NULL, NULL) == 0, "openpty");
    CHECK(!strncmp(name, "/dev/pts/", 9), "the slave is /dev/pts/<n>");
    CHECK(isatty(s) && isatty(m), "both sides are terminals");
    char tn[64];
    CHECK(ttyname_r(s, tn, sizeof tn) == 0 && !strcmp(tn, name), "ttyname() of the slave");
    struct stat st;
    CHECK(stat(name, &st) == 0 && S_ISCHR(st.st_mode) && (st.st_rdev >> 8) == 136, "stat: character device (136, n)");

    /* canonical mode: the line arrives at the end, the echo goes back to the master */
    write(m, "hola\r", 5);
    char buf[256];
    int n = read(s, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    CHECK(n == 5 && !strcmp(buf, "hola\n"), "a line typed on the master is read on the slave (\\r -> \\n)");
    n = read_some(m, buf, sizeof buf - 1, 200);
    CHECK(strstr(buf, "hola\r\n") != NULL, "the echo comes back to the master (\\n -> \\r\\n)");
    write(m, "ab\x7f" "c\n", 5);
    n = read(s, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    CHECK(!strcmp(buf, "ac\n"), "erase (DEL) edits the line");
    read_some(m, buf, sizeof buf - 1, 100);

    /* output of the slave */
    write(s, "x\n", 2);
    n = read_some(m, buf, sizeof buf - 1, 200);
    CHECK(!strcmp(buf, "x\r\n"), "what the slave writes is read on the master");

    /* raw mode */
    struct termios t, orig;
    tcgetattr(s, &orig);
    t = orig;
    cfmakeraw(&t);
    tcsetattr(s, TCSANOW, &t);
    write(m, "\x03q", 2);
    n = read_some(s, buf, 2, 200);
    CHECK(n == 2 && buf[0] == 3 && buf[1] == 'q', "raw mode: bytes as they are, Ctrl-C is a byte");

    struct winsize ws = { .ws_row = 40, .ws_col = 132 };
    CHECK(ioctl(m, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ on the master");
    struct winsize ws2;
    CHECK(ioctl(s, TIOCGWINSZ, &ws2) == 0 && ws2.ws_row == 40 && ws2.ws_col == 132, "the slave sees the size");
    tcsetattr(s, TCSANOW, &orig);                 /* canonical again, with ISIG */
    close(s);

    printf("== sessions and controlling terminals ==\n");
    /* A child: a new session, the slave as its controlling terminal; Ctrl-C from the master
     * reaches it; /dev/tty is the slave. Then the master is closed: SIGHUP */
    int pfd[2];
    pipe(pfd);
    pid_t c = fork();
    if (c == 0) {
        close(pfd[0]);
        close(m);
        setsid();
        int fd = open(name, O_RDWR);              /* a session leader: it becomes its terminal */
        int ok = fd >= 0;
        int tfd = open("/dev/tty", O_RDWR);
        ok = ok && tfd >= 0;
        ok = ok && tcgetpgrp(fd) == getpgrp();
        write(pfd[1], ok ? "Y" : "N", 1);
        signal(SIGINT, SIG_DFL);
        for (;;) pause();                         /* until a signal ends it */
    }
    close(pfd[1]);
    char r = 0;
    read(pfd[0], &r, 1);
    CHECK(r == 'Y', "setsid + opening the pty: controlling terminal, /dev/tty, tcgetpgrp");
    write(m, "\x03", 1);
    int status = 0;
    pid_t w = 0;
    for (int i = 0; i < 50 && w == 0; i++) { w = waitpid(c, &status, WNOHANG); if (!w) usleep(20000); }
    CHECK(w == c && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT, "Ctrl-C on the master: SIGINT to the foreground group");

    c = fork();
    if (c == 0) {
        close(m);
        setsid();
        open(name, O_RDWR);
        signal(SIGHUP, SIG_DFL);
        for (;;) pause();
    }
    usleep(100000);
    close(m);                                     /* hangup */
    w = 0;
    for (int i = 0; i < 50 && w == 0; i++) { w = waitpid(c, &status, WNOHANG); if (!w) usleep(20000); }
    CHECK(w == c && WIFSIGNALED(status) && WTERMSIG(status) == SIGHUP, "closing the master: SIGHUP to the session");

    /* Two at once: each its own number (TIOCGPTN), its own data */
    int m2, s2;
    char name2[64];
    CHECK(openpty(&m, &s, name, NULL, NULL) == 0 && openpty(&m2, &s2, name2, NULL, NULL) == 0 &&
          strcmp(name, name2) != 0, "two ptys at once: different names");
    write(s2, "two\n", 4);
    n = read_some(m2, buf, sizeof buf - 1, 200);
    int none = read_some(m, buf + 100, 50, 100);
    CHECK(strstr(buf, "two") && none == 0, "each master gets its own slave's output");
    close(s); close(m); close(s2); close(m2);

    /* The slave closed: the master reads EIO */
    CHECK(openpty(&m, &s, name, NULL, NULL) == 0, "openpty again");
    close(s);
    n = read(m, buf, 1);
    CHECK(n < 0 && errno == EIO, "the slave closed: the master reads EIO");
    close(m);

    printf("== %s ==\n", failures ? "THERE ARE FAILURES" : "all good");
    return failures ? 1 : 0;
}
