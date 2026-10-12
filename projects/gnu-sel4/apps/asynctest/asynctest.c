/*
 * asynctest: signal handlers reach a process in a loop that makes no system calls
 * (POSIX_SYS_ASYNC: server/proc.c, runtime/crt0.S), and the loop goes on with its registers,
 * integer and floating point, as they were. Each check on its own line.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
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

    printf("== %s ==\n", failures ? "THERE ARE FAILURES" : "all good");
    return failures ? 1 : 0;
}
