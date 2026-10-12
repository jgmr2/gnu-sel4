/*
 * limittest: resource limits (server/proc.c): RLIMIT_NOFILE, RLIMIT_AS (the page budget),
 * RLIMIT_NPROC, and who may raise them. Each check on its own line.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
                               else { printf("  FAIL %s (errno %d: %s)\n", what, errno, strerror(errno)); failures++; } } while (0)

static int child_status(void (*fn)(void))
{
    pid_t c = fork();
    if (c == 0) { fn(); _exit(0); }
    int st = 0;
    waitpid(c, &st, 0);
    return st;
}

static void nofile(void)
{
    struct rlimit r = { 10, 10 };
    if (setrlimit(RLIMIT_NOFILE, &r)) _exit(2);
    int last = -1, err = 0;
    for (int i = 0; i < 20; i++) {
        int fd = open("/etc/passwd", O_RDONLY);
        if (fd < 0) { err = errno; break; }
        last = fd;
    }
    _exit(last == 9 && err == EMFILE ? 0 : 1);
}

static void raise_hard(void)
{
    if (setuid(1000)) _exit(2);
    struct rlimit r;
    getrlimit(RLIMIT_NPROC, &r);
    r.rlim_max += 10;
    _exit(setrlimit(RLIMIT_NPROC, &r) == -1 && errno == EPERM ? 0 : 1);
}

static void address_space(void)
{
    struct rlimit r = { 16 << 20, 16 << 20 };
    if (setrlimit(RLIMIT_AS, &r)) _exit(2);
    volatile char *p = malloc(64 << 20);       /* volatile: or the compiler drops the stores */
    if (!p) _exit(3);                          /* refused up front: fine too */
    for (size_t i = 0; i < (64u << 20); i += 4096) p[i] = 1;   /* the 16 MiB line: killed */
    _exit(1);
}

static void nproc(void)
{
    if (setuid(1000)) _exit(2);
    struct rlimit r = { 3, 3 };
    if (setrlimit(RLIMIT_NPROC, &r)) _exit(2);
    int made = 0, err = 0;
    pid_t kids[8];
    for (int i = 0; i < 8; i++) {
        pid_t c = fork();
        if (c == 0) { pause(); _exit(0); }
        if (c < 0) { err = errno; break; }
        kids[made++] = c;
    }
    for (int i = 0; i < made; i++) kill(kids[i], SIGKILL);
    while (wait(NULL) > 0) ;
    _exit(made == 2 && err == EAGAIN ? 0 : 1);   /* itself + 2 = 3 */
}

int main(void)
{
    printf("== resource limits ==\n");
    struct rlimit r;
    CHECK(getrlimit(RLIMIT_NOFILE, &r) == 0 && r.rlim_cur == 256 && r.rlim_max == 256, "RLIMIT_NOFILE is the descriptor table (256)");
    CHECK(getdtablesize() == 256, "getdtablesize() follows it");
    CHECK(getrlimit(RLIMIT_STACK, &r) == 0 && r.rlim_cur == 8 << 20, "RLIMIT_STACK is the 8 MiB stack");
    int st = child_status(nofile);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "NOFILE 10: the 11th descriptor fails with EMFILE");
    st = child_status(raise_hard);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a user cannot raise a hard limit (EPERM)");
    st = child_status(address_space);
    CHECK((WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL) || (WIFEXITED(st) && WEXITSTATUS(st) == 3),
          "RLIMIT_AS 16 MiB: touching 64 MiB stops the process");
    st = child_status(nproc);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "RLIMIT_NPROC 3: the third fork fails with EAGAIN");
    printf("== %s ==\n", failures ? "THERE ARE FAILURES" : "all good");
    return failures ? 1 : 0;
}
