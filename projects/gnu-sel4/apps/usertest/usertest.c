/*
 * usertest: users and groups (run as root by make test). A child drops to uid/gid 1000 and
 * checks what it can and cannot do; a set-user-ID root copy of this program gives it euid 0.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, what) do { \
    if (cond) printf("  ok   %s\n", what); \
    else { printf("  FAIL %s (errno %d: %s)\n", what, errno, strerror(errno)); failures++; } \
    fflush(stdout); \
} while (0)

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "setuid-child")) {    /* exec'd set-user-ID root */
        uid_t r, e, s;
        getresuid(&r, &e, &s);
        int ok = r == 1000 && e == 0 && s == 0;
        ok = ok && seteuid(1000) == 0 && geteuid() == 1000 && seteuid(0) == 0 && geteuid() == 0;   /* saved uid */
        return ok ? 0 : 1;
    }
    printf("== users and groups ==\n");
    CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0, "init's descendants start as root");
    mkdir("/tmp/ut", 01777);
    chmod("/tmp/ut", 01777);

    /* a set-user-ID root copy of this program */
    int in = open("/bin/usertest", O_RDONLY), out = open("/tmp/ut/suid", O_WRONLY | O_CREAT | O_TRUNC, 0755);
    char buf[65536];
    ssize_t n;
    while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof buf)) > 0) write(out, buf, n);
    close(in); close(out);
    CHECK(chmod("/tmp/ut/suid", 04755) == 0, "chmod 4755 (set-user-ID)");

    pid_t pid = fork();
    if (pid == 0) {
        gid_t g[2] = { 100, 1000 };
        int ok = setgroups(2, g) == 0 && setgid(1000) == 0 && setuid(1000) == 0;
        if (!ok) _exit(10);
        if (getuid() != 1000 || geteuid() != 1000 || getgid() != 1000) _exit(11);
        if (setuid(0) == 0 || errno != EPERM) _exit(12);               /* no way back */
        gid_t got[8];
        if (getgroups(8, got) != 2 || got[0] != 100 || got[1] != 1000) _exit(13);
        if (setgroups(0, NULL) == 0) _exit(14);                         /* only root */
        int fd = open("/tmp/ut/mine", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || st.st_uid != 1000 || st.st_gid != 1000) _exit(15);
        close(fd);
        if (kill(1, SIGTERM) == 0 || errno != EPERM) _exit(16);         /* not someone else's */
        pid_t q = fork();
        if (q == 0) execl("/tmp/ut/suid", "suid", "setuid-child", (char *)0), _exit(17);
        int st2;
        waitpid(q, &st2, 0);
        _exit(WIFEXITED(st2) && WEXITSTATUS(st2) == 0 ? 0 : 18);
    }
    int st;
    waitpid(pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    const char *why[] = { "", "", "", "", "", "", "", "", "", "",
        "setgroups/setgid/setuid", "IDs after setuid(1000)", "setuid(0) back must fail", "getgroups",
        "setgroups without privilege must fail", "a new file belongs to its creator", "kill(init) must fail",
        "exec of the set-user-ID program", "set-user-ID: euid 0 and the saved uid" };
    if (code) printf("       child failed at: %s\n", code > 0 && code < 19 ? why[code] : "?");
    CHECK(code == 0, "a process drops to uid 1000: no way back, owns its files, cannot signal init, set-user-ID works");
    struct stat ms;
    CHECK(stat("/tmp/ut/mine", &ms) == 0 && ms.st_uid == 1000, "the file stays owned by 1000");
    unlink("/tmp/ut/mine"); unlink("/tmp/ut/suid"); rmdir("/tmp/ut");
    printf("== %s ==\n", failures ? "THERE ARE FAILURES" : "all good");
    return failures;
}
