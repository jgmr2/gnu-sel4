/*
 * jobtest: job control (server/proc.c, server/tty.c): stopping and continuing processes,
 * wait4 with WUNTRACED and WCONTINUED, Ctrl-Z, SIGTTIN. Each check on its own line.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, what) do { if (cond) printf("  ok   %s\n", what); \
                               else { printf("  FAIL %s (errno %d: %s)\n", what, errno, strerror(errno)); failures++; } } while (0)

/* waitpid with options, up to ~2 s */
static pid_t wait_for(pid_t pid, int *status, int options)
{
    for (int i = 0; i < 100; i++) {
        pid_t w = waitpid(pid, status, options | WNOHANG);
        if (w != 0) return w;
        usleep(20000);
    }
    return 0;
}

static char proc_state(pid_t pid)
{
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return '?';
    int n = read(fd, buf, sizeof buf - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    char *p = strrchr(buf, ')');
    return p && p[1] == ' ' ? p[2] : '?';
}

int main(void)
{
    int status;
    printf("== stopping and continuing ==\n");
    pid_t c = fork();
    if (c == 0) {
        int r = kill(getpid(), SIGSTOP);       /* returns once, after SIGCONT */
        _exit(r == 0 ? 7 : 1);
    }
    CHECK(wait_for(c, &status, WUNTRACED) == c && WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP,
          "a process that stops itself: WUNTRACED reports SIGSTOP");
    CHECK(proc_state(c) == 'T', "/proc shows it stopped (T)");
    kill(c, SIGCONT);
    CHECK(wait_for(c, &status, WCONTINUED) == c && WIFCONTINUED(status), "SIGCONT: WCONTINUED reports it");
    CHECK(wait_for(c, &status, 0) == c && WIFEXITED(status) && WEXITSTATUS(status) == 7,
          "it goes on where it stopped (kill returned 0, once)");

    c = fork();
    if (c == 0) for (volatile unsigned long i = 0;; i++) ;   /* no system calls at all */
    usleep(50000);
    kill(c, SIGSTOP);
    CHECK(wait_for(c, &status, WUNTRACED) == c && WIFSTOPPED(status), "a pure loop is stopped too");
    kill(c, SIGCONT);
    CHECK(wait_for(c, &status, WCONTINUED) == c && WIFCONTINUED(status), "and continued");
    kill(c, SIGKILL);
    CHECK(wait_for(c, &status, 0) == c && WIFSIGNALED(status), "and killed");

    printf("== the terminal: Ctrl-Z, background reads ==\n");
    int m, s;
    char name[64];
    if (openpty(&m, &s, name, NULL, NULL)) { perror("openpty"); return 1; }
    int pfd[2];
    pipe(pfd);
    c = fork();
    if (c == 0) {                              /* a session on the pty, like a shell */
        close(m); close(s); close(pfd[0]);
        setsid();
        int fd = open(name, O_RDWR);
        /* a "job" in its own group, in the foreground: Ctrl-Z stops it */
        pid_t job = fork();
        if (job == 0) { setpgid(0, 0); for (;;) pause(); }
        setpgid(job, job);
        tcsetpgrp(fd, job);
        write(pfd[1], "R", 1);                 /* ready for the Ctrl-Z */
        int st;
        char res = wait_for(job, &st, WUNTRACED) == job && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP ? 'Z' : 'z';
        write(pfd[1], &res, 1);
        kill(job, SIGKILL);
        waitpid(job, &st, 0);
        /* a background job reading the terminal: SIGTTIN stops it */
        tcsetpgrp(fd, getpgrp());
        job = fork();
        if (job == 0) { setpgid(0, 0); char b; read(fd, &b, 1); _exit(0); }
        setpgid(job, job);
        res = wait_for(job, &st, WUNTRACED) == job && WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTIN ? 'T' : 't';
        write(pfd[1], &res, 1);
        kill(job, SIGKILL);
        waitpid(job, &st, 0);
        _exit(0);
    }
    close(s); close(pfd[1]);
    char r = 0;
    read(pfd[0], &r, 1);
    usleep(50000);
    write(m, "\x1a", 1);                       /* Ctrl-Z */
    read(pfd[0], &r, 1);
    CHECK(r == 'Z', "Ctrl-Z on the terminal: SIGTSTP stops the foreground job");
    read(pfd[0], &r, 1);
    CHECK(r == 'T', "a background job reading its terminal: stopped by SIGTTIN");
    wait_for(c, &status, 0);
    close(m);

    printf("== %s ==\n", failures ? "THERE ARE FAILURES" : "all good");
    return failures ? 1 : 0;
}
