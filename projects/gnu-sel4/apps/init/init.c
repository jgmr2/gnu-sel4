/*
 * GNU/seL4 init (pid 1): runs /etc/rc (services, such as 9pd) and keeps a
 * login prompt (/bin/login) on the console, starting it again when the session
 * ends. Without /bin/login: root's shell from /etc/passwd (or /bin/sh).
 * posix_server reaps orphaned processes, so no wait is needed here.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static pid_t run(const char *path, char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        execv(path, argv);
        perror(path);
        _exit(127);
    }
    return pid;
}

int main(void)
{
    if (getpid() != 1) {
        fprintf(stderr, "init: must be process 1\n");
        return 1;
    }
    if (access("/etc/rc", R_OK) == 0) {
        char *rc[] = { "sh", "/etc/rc", NULL };
        pid_t pid = run("/bin/sh", rc);
        while (pid > 0 && waitpid(pid, NULL, 0) < 0) ;
    }
    for (;;) {
        /* the console: login (it becomes the user and starts its shell); without it, root's
         * shell per /etc/passwd (argv[0] starting with "-" = login shell) */
        static char shell[256], arg0[64];
        char *login[] = { "login", NULL };
        pid_t pid;
        if (access("/bin/login", X_OK) == 0) {
            pid = run("/bin/login", login);
        } else {
            struct passwd *pw = getpwuid(0);
            snprintf(shell, sizeof shell, "%s", pw && pw->pw_shell && access(pw->pw_shell, X_OK) == 0 ? pw->pw_shell : "/bin/sh");
            const char *base = strrchr(shell, '/') ? strrchr(shell, '/') + 1 : shell;
            snprintf(arg0, sizeof arg0, "-%s", base);
            char *sh[] = { arg0, NULL };
            pid = run(shell, sh);
        }
        if (pid < 0) { sleep(1); continue; }
        int st;
        while (wait(&st) != pid) ;
        sleep(1);                           /* avoid a tight loop if the shell fails */
    }
}
