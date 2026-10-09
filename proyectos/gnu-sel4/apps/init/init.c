/*
 * init de GNU/seL4 (pid 1): ejecuta /etc/rc (servicios, como 9pd) y mantiene
 * una shell de sesión en la consola, volviéndola a lanzar si termina. La shell
 * es la de root en /etc/passwd (si no existe, /bin/sh).
 * Los procesos huérfanos los recoge posix_server, no hace falta wait aquí.
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
        fprintf(stderr, "init: debe ser el proceso 1\n");
        return 1;
    }
    if (access("/etc/rc", R_OK) == 0) {
        char *rc[] = { "sh", "/etc/rc", NULL };
        pid_t pid = run("/bin/sh", rc);
        while (pid > 0 && waitpid(pid, NULL, 0) < 0) ;
    }
    for (;;) {
        /* shell de root según /etc/passwd; argv[0] con "-" = shell de sesión */
        static char shell[256], arg0[64];
        struct passwd *pw = getpwuid(0);
        snprintf(shell, sizeof shell, "%s", pw && pw->pw_shell && access(pw->pw_shell, X_OK) == 0 ? pw->pw_shell : "/bin/sh");
        const char *base = strrchr(shell, '/') ? strrchr(shell, '/') + 1 : shell;
        snprintf(arg0, sizeof arg0, "-%s", base);
        char *sh[] = { arg0, NULL };
        pid_t pid = run(shell, sh);
        if (pid < 0) { sleep(1); continue; }
        int st;
        while (wait(&st) != pid) ;
        sleep(1);                           /* evitar un bucle rápido si la shell falla */
    }
}
