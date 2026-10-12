/*
 * poweroff, reboot, halt: an orderly shutdown of GNU/seL4.
 *   1. s6-rc brings every service down (in reverse dependency order)
 *   2. the Linux VM runs its own shutdown (stops Docker, syncs and unmounts its disk)
 *   3. every other process gets SIGTERM, then SIGKILL
 *   4. reboot(2): posix_server unmounts the file system clean and powers off / restarts / halts
 *   poweroff -f (or reboot -f, halt -f): only step 4
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/wait.h>

static void on_alarm(int s) { (void)s; }

/* Runs a command and waits for it, at most `seconds` (then it is killed) */
static void run(int seconds, char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pid < 0) return;
    struct sigaction sa = { .sa_handler = on_alarm };
    sigaction(SIGALRM, &sa, NULL);
    alarm(seconds);
    if (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        fprintf(stderr, "%s: %s did not finish in %d s, going on\n", "poweroff", argv[0], seconds);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    alarm(0);
}

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    int cmd = !strcmp(name, "reboot") ? RB_AUTOBOOT : !strcmp(name, "halt") ? RB_HALT_SYSTEM : RB_POWER_OFF;
    int force = argc > 1 && !strcmp(argv[1], "-f");
    if (geteuid() != 0) {
        fprintf(stderr, "%s: must be root\n", name);
        return 1;
    }
    if (!force) {
        printf("%s: stopping the services\n", name);
        fflush(stdout);
        run(30, (char *const[]){ "s6-rc", "-bda", "change", NULL });
        if (access("/dev/linux", F_OK) == 0) {
            printf("%s: shutting down the Linux VM\n", name);
            fflush(stdout);
            run(20, (char *const[]){ "linux", "/etc/rc.shutdown", NULL });
        }
        /* Everyone else goes, sudo and the login shell among them: when the session leader dies
         * its terminal's foreground group (this one) gets SIGHUP, and with sudo's use_pty the
         * master side closes. Neither may stop us before reboot() */
        signal(SIGTERM, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        kill(-1, SIGTERM);
        sleep(1);
        kill(-1, SIGKILL);
    }
    sync();
    reboot(cmd);
    perror(name);                          /* only if posix_server refused */
    return 1;
}
