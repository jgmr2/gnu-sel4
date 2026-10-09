/* Prueba de procesos: fork, exec, wait, tuberías, dup2, señales y fallos. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

static int fails;
#define CHECK(cond, what) do { \
    if (cond) printf("  ok   %s\n", what); \
    else { printf("  FALLA %s (errno %d: %s)\n", what, errno, strerror(errno)); fails++; } \
    fflush(stdout); \
} while (0)

static volatile int got_usr1, got_chld;
static void on_usr1(int s) { (void)s; got_usr1++; }
static void on_chld(int s) { (void)s; got_chld++; }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "hijo")) {       /* modo hijo para exec */
        printf("hijo exec: pid=%d ppid=%d args=%d\n", getpid(), getppid(), argc);
        return 7;
    }
    int st;
    printf("== fork / wait ==\n");
    pid_t p = fork();
    if (p == 0) _exit(42);
    CHECK(p > 0 && waitpid(p, &st, 0) == p && WIFEXITED(st) && WEXITSTATUS(st) == 42, "fork + _exit(42) + waitpid");

    int x = 1;
    p = fork();
    if (p == 0) { x = 99; _exit(getppid() == getppid() ? 0 : 1); }
    waitpid(p, &st, 0);
    CHECK(x == 1, "la memoria del hijo es independiente");

    printf("== exec ==\n");
    fflush(stdout);
    p = fork();
    if (p == 0) {
        char *av[] = { "proctest", "hijo", "a", "b", NULL };
        execv("/bin/proctest", av);
        _exit(99);
    }
    CHECK(waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 7, "fork + execv + código 7");
    CHECK(execl("/bin/no-existe", "x", (char *)0) < 0 && errno == ENOENT, "exec inexistente -> ENOENT");

    printf("== tuberías ==\n");
    int fd[2];
    CHECK(pipe(fd) == 0, "pipe");
    p = fork();
    if (p == 0) {
        close(fd[0]);
        char buf[1000];
        memset(buf, 'z', sizeof buf);
        for (int i = 0; i < 100; i++) write(fd[1], buf, sizeof buf);   /* 100 KB > búfer de la tubería */
        _exit(0);
    }
    close(fd[1]);
    long total = 0;
    ssize_t n;
    char rb[4096];
    while ((n = read(fd[0], rb, sizeof rb)) > 0) total += n;
    close(fd[0]);
    waitpid(p, &st, 0);
    CHECK(total == 100000 && n == 0, "100 KB por una tubería + EOF al cerrar");

    pipe(fd);
    p = fork();
    if (p == 0) {
        dup2(fd[1], 1);
        close(fd[0]); close(fd[1]);
        execl("/bin/seq", "seq", "1", "5", (char *)0);
        _exit(99);
    }
    close(fd[1]);
    FILE *f = fdopen(fd[0], "r");
    int lines = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) lines++;
    fclose(f);
    waitpid(p, &st, 0);
    CHECK(lines == 5, "dup2 + exec: salida de /bin/seq capturada por tubería");

    printf("== señales ==\n");
    signal(SIGUSR1, on_usr1);
    signal(SIGCHLD, on_chld);
    kill(getpid(), SIGUSR1);
    CHECK(got_usr1 == 1, "manejador de SIGUSR1 (kill a sí mismo)");
    pipe(fd);
    p = fork();
    if (p == 0) { char c; read(fd[0], &c, 1); _exit(0); }  /* se bloquea leyendo */
    CHECK(kill(p, SIGTERM) == 0 && waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM,
          "kill(SIGTERM) a un hijo bloqueado");
    close(fd[0]); close(fd[1]);
    CHECK(got_chld >= 1, "SIGCHLD recibido");
    p = fork();
    if (p == 0) { *(volatile int *)0 = 1; _exit(0); }
    CHECK(waitpid(p, &st, 0) == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "fallo de página -> SIGSEGV");

    printf("== varios procesos a la vez ==\n");
    pid_t kids[6];
    for (int i = 0; i < 6; i++) if ((kids[i] = fork()) == 0) { usleep(0); _exit(i); }
    int ok = 1;
    for (int i = 0; i < 6; i++) ok &= waitpid(kids[i], &st, 0) == kids[i] && WEXITSTATUS(st) == i;
    CHECK(ok, "6 hijos simultáneos");
    CHECK(wait(&st) < 0 && errno == ECHILD, "wait sin hijos -> ECHILD");

    printf("== %s ==\n", fails ? "HAY FALLAS" : "todo bien");
    return fails;
}
