/*
 * shelltest: job control in real shells. zsh and dash run interactively on a pseudo-terminal
 * and are typed at like a user: a job stopped with Ctrl-Z, jobs, bg, fg, Ctrl-C; Ctrl-C in the
 * middle of a line gives a new prompt at once. Each check on its own line.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static char out[65536];
static size_t outlen;

static void check(int cond, const char *shell, const char *what)
{
    printf("  %s %s: %s\n", cond ? "ok  " : "FAIL", shell, what);
    if (!cond) failures++;
}

/* Reads from the master until text has appeared after mark (or ms pass) */
static int expect(int m, size_t mark, const char *text, int ms)
{
    for (int waited = 0; waited <= ms; waited += 20) {
        out[outlen] = 0;
        if (strstr(out + mark, text)) return 1;
        struct pollfd p = { .fd = m, .events = POLLIN };
        if (poll(&p, 1, 20) > 0) {
            ssize_t n = read(m, out + outlen, sizeof out - 1 - outlen);
            if (n > 0) outlen += n;
            else if (n < 0 && errno != EINTR) return 0;
        }
    }
    out[outlen] = 0;
    return strstr(out + mark, text) != NULL;
}

static void type(int m, const char *s) { write(m, s, strlen(s)); }

static void run(const char *shell, char *const argv[], const char *stopped)
{
    int m;
    pid_t pid = forkpty(&m, NULL, NULL, NULL);
    if (pid == 0) {
        setenv("PS1", "P> ", 1);
        setenv("PROMPT", "P> ", 1);
        setenv("TERM", "dumb", 1);
        execv(argv[0], argv);
        _exit(127);
    }
    outlen = 0;
    check(expect(m, 0, "P> ", 3000), shell, "an interactive prompt on the pty");

    size_t mark = outlen;
    type(m, "sleep 30\r");
    usleep(300000);
    type(m, "\x1a");                                   /* Ctrl-Z */
    check(expect(m, mark, stopped, 3000) && expect(m, mark, "P> ", 2000), shell, "Ctrl-Z stops the job, the prompt is back");

    mark = outlen;
    type(m, "jobs\r");
    check(expect(m, mark, "sleep 30", 2000), shell, "jobs lists it");

    mark = outlen;
    type(m, "bg\r");
    expect(m, mark, "P> ", 2000);
    mark = outlen;
    type(m, "fg\r");
    usleep(300000);
    type(m, "\x03");                                   /* Ctrl-C to the job in the foreground */
    check(expect(m, mark, "P> ", 3000), shell, "bg, fg, then Ctrl-C ends it");

    mark = outlen;
    type(m, "echo half a line");
    usleep(200000);
    type(m, "\x03");                                   /* Ctrl-C in the middle of the line */
    check(expect(m, mark, "P> ", 1500), shell, "Ctrl-C in the middle of a line: a new prompt at once");

    mark = outlen;
    type(m, "echo sum-$((40+2))\r");
    check(expect(m, mark, "sum-42", 2000), shell, "and the shell still works");

    type(m, "exit\r");
    usleep(200000);
    close(m);
    int st;
    for (int i = 0; i < 50 && waitpid(pid, &st, WNOHANG) == 0; i++) usleep(20000);
    kill(pid, SIGKILL);
    waitpid(pid, &st, WNOHANG);
    if (failures) {                                    /* what the shell showed, for the log */
        printf("    --- %s on the pty ---\n", shell);
        for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) printf("    | %s\n", l);
    }
}

int main(void)
{
    printf("== job control in the shells ==\n");
    char *zsh[] = { "/bin/zsh", "-f", "-i", NULL };
    run("zsh", zsh, "suspended");
    char *dash[] = { "/bin/dash", "-i", NULL };
    run("dash", dash, "Stopped");
    printf("== %s ==\n", failures ? "THERE ARE FAILURES" : "all good");
    return failures ? 1 : 0;
}
