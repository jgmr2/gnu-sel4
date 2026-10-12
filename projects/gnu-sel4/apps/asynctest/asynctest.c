/*
 * asynctest: signal handlers reach a process in a loop that makes no system calls
 * (POSIX_SYS_ASYNC: server/proc.c, runtime/crt0.S), and the loop goes on with its registers,
 * integer and floating point, as they were. Each check on its own line.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
                               else { printf("  FAIL %s (errno %d: %s)\n", what, errno, strerror(errno)); failures++; } } while (0)

static volatile sig_atomic_t got;
static int devnull = -1;

/* A handler that makes calls of its own, in the middle of the program's (dropbear's SIGCHLD
 * handler writes to a pipe) */
static void busy_handler(int sig)
{
    (void)sig;
    char c = 1;
    write(devnull, &c, 1);
    getpid();
}
static volatile double sink;

static void handler(int sig)
{
    double z = 0;                              /* the handler uses the FPU too */
    for (int k = 0; k < 200; k++) z += k * 0.37;
    sink = z;
    got = sig;
}

/* A loop with no calls until the signal; its invariants tell whether a register changed */
static int loop_until_signal(void)
{
    unsigned long i = 0, j = 0;
    double a = 0;
    while (!got) {
        i++;
        j += 3;
        a += 1.5;
    }
    return j == 3 * i && a == 1.5 * (double)i;
}

int main(void)
{
    printf("== asynchronous signals ==\n");
    signal(SIGUSR1, handler);
    pid_t c = fork();
    if (c == 0) _exit(loop_until_signal() ? 0 : 2);
    usleep(100000);
    kill(c, SIGUSR1);
    int status = 0;
    pid_t w = 0;
    for (int k = 0; k < 100 && w == 0; k++) { w = waitpid(c, &status, WNOHANG); if (!w) usleep(20000); }
    if (w == 0) { kill(c, SIGKILL); waitpid(c, &status, 0); }
    CHECK(w == c && WIFEXITED(status), "kill: the handler runs in a loop without system calls");
    CHECK(w == c && WIFEXITED(status) && WEXITSTATUS(status) == 0, "the loop's registers (integer, SSE) are intact");

    got = 0;
    signal(SIGALRM, handler);
    struct itimerval it = { .it_value = { .tv_usec = 100000 } };
    setitimer(ITIMER_REAL, &it, NULL);
    int ok = loop_until_signal();              /* in this process: the timer's SIGALRM */
    CHECK(got == SIGALRM, "setitimer: SIGALRM interrupts a loop of this process");
    CHECK(ok, "and its registers are intact");

    /* 512 KiB through a pipe that fills up (the writes wait with their data in the shared
     * buffer) while the writer takes signal after signal: not one byte changes */
    devnull = open("/dev/null", O_WRONLY);
    int pfd[2];
    pipe(pfd);
    c = fork();
    if (c == 0) {
        signal(SIGUSR1, busy_handler);
        close(pfd[0]);
        static unsigned char chunk[8192];
        for (int k = 0; k < 64; k++) {
            for (int b = 0; b < (int)sizeof chunk; b++) chunk[b] = (unsigned char)(k * 7 + b * 13 + 5);
            for (size_t off = 0; off < sizeof chunk; ) {
                ssize_t w = write(pfd[1], chunk + off, sizeof chunk - off);
                if (w > 0) off += w;
            }
        }
        _exit(0);
    }
    close(pfd[1]);
    static unsigned char got_data[8192 * 64];
    size_t total = 0;
    int sent = 0;
    while (total < sizeof got_data) {
        ssize_t r = read(pfd[0], got_data + total, sizeof got_data - total < 1024 ? sizeof got_data - total : 1024);
        if (r <= 0) break;
        total += r;
        if (sent < 400) { kill(c, SIGUSR1); sent++; }
    }
    int same = total == sizeof got_data;
    for (size_t b = 0; same && b < total; b++)
        same = got_data[b] == (unsigned char)((b / 8192) * 7 + (b % 8192) * 13 + 5);
    waitpid(c, &status, 0);
    CHECK(same, "a handler's own calls do not change the call it interrupted (512 KiB, 400 signals)");

    printf("== %s ==\n", failures ? "THERE ARE FAILURES" : "all good");
    return failures ? 1 : 0;
}
