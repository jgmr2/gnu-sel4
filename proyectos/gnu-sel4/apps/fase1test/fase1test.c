/*
 * fase1test: lo que Wayland le pide a POSIX (TODO, punto 6, Fase 1).
 *   epoll, eventfd, timerfd; sockets AF_UNIX con paso de descriptores (SCM_RIGHTS);
 *   memoria compartida (memfd_create + mmap MAP_SHARED) y mmap de archivos
 */
#define _GNU_SOURCE          /* struct ucred */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/mman.h>

static int fallos;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FALLA", what);
    if (!ok) fallos++;
}

static long ms_since(struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

int main(void)
{
    printf("== fase1test: epoll, eventfd, timerfd ==\n");
    uint64_t v;

    /* eventfd */
    int e = eventfd(0, EFD_NONBLOCK);
    check(e >= 0, "eventfd");
    check(read(e, &v, 8) < 0 && errno == EAGAIN, "eventfd vacío -> EAGAIN");
    v = 3; write(e, &v, 8); v = 4; write(e, &v, 8);
    check(read(e, &v, 8) == 8 && v == 7, "eventfd suma escrituras (3 + 4 = 7)");

    /* epoll + pipe */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    check(ep >= 0, "epoll_create1");
    int p[2];
    pipe(p);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0xabcd };
    check(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) == 0, "epoll_ctl ADD");
    check(epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) < 0 && errno == EEXIST, "epoll_ctl ADD repetido -> EEXIST");
    struct epoll_event out[4];
    check(epoll_wait(ep, out, 4, 0) == 0, "epoll_wait sin datos -> 0");
    write(p[1], "x", 1);
    int n = epoll_wait(ep, out, 4, 1000);
    check(n == 1 && (out[0].events & EPOLLIN) && out[0].data.u64 == 0xabcd, "epoll_wait: tubería lista con su data");
    char c;
    read(p[0], &c, 1);

    /* timerfd de una vez, esperado con epoll */
    int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    check(t >= 0, "timerfd_create");
    ev = (struct epoll_event){ .events = EPOLLIN, .data.fd = t };
    epoll_ctl(ep, EPOLL_CTL_ADD, t, &ev);
    struct itimerspec its = { .it_value = { 0, 50 * 1000000 } };
    timerfd_settime(t, 0, &its, NULL);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    n = epoll_wait(ep, out, 4, 2000);
    long dt = ms_since(&t0);
    check(n == 1 && out[0].data.fd == t && dt >= 40 && dt < 500, "epoll_wait despierta con timerfd (50 ms)");
    printf("        %ld ms\n", dt);
    check(read(t, &v, 8) == 8 && v == 1, "timerfd: 1 expiración");

    /* timerfd periódico */
    its = (struct itimerspec){ .it_value = { 0, 10 * 1000000 }, .it_interval = { 0, 10 * 1000000 } };
    timerfd_settime(t, 0, &its, NULL);
    usleep(105 * 1000);
    check(read(t, &v, 8) == 8 && v >= 8 && v <= 12, "timerfd periódico (10 ms) tras 105 ms: ~10 expiraciones");
    printf("        %llu expiraciones\n", (unsigned long long)v);
    struct itimerspec cur;
    timerfd_gettime(t, &cur);
    check(cur.it_interval.tv_nsec == 10 * 1000000, "timerfd_gettime da el intervalo");

    /* eventfd entre procesos: el hijo despierta al padre */
    epoll_ctl(ep, EPOLL_CTL_DEL, t, NULL);
    int e2 = eventfd(0, 0);
    ev = (struct epoll_event){ .events = EPOLLIN, .data.fd = e2 };
    epoll_ctl(ep, EPOLL_CTL_ADD, e2, &ev);
    pid_t pid = fork();
    if (pid == 0) {
        usleep(30 * 1000);
        v = 1;
        write(e2, &v, 8);
        _exit(0);
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    n = epoll_wait(ep, out, 4, 2000);
    dt = ms_since(&t0);
    check(n == 1 && out[0].data.fd == e2 && dt >= 20, "eventfd: el hijo despierta al padre en epoll_wait");
    waitpid(pid, NULL, 0);

    close(ep);

    /* AF_UNIX: servidor en una ruta, cliente en un hijo, paso de un descriptor */
    printf("-- AF_UNIX --\n");
    int sp[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0, "socketpair");
    write(sp[0], "hola", 4);
    char buf[16] = { 0 };
    check(read(sp[1], buf, 16) == 4 && !memcmp(buf, "hola", 4), "socketpair: datos de un extremo al otro");
    close(sp[0]);
    check(read(sp[1], buf, 16) == 0, "cerrar un extremo -> EOF en el otro");
    close(sp[1]);

    const char *path = "/tmp/fase1.sock";
    unlink(path);
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strcpy(sa.sun_path, path);
    check(bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(ls, 4) == 0, "bind + listen en /tmp/fase1.sock");
    struct stat st;
    check(stat(path, &st) == 0 && S_ISSOCK(st.st_mode), "stat ve el socket");
    pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&sa, sizeof sa)) _exit(1);
        int pf[2];
        pipe(pf);
        write(pf[1], "por el fd", 9);
        char cbuf[CMSG_SPACE(sizeof(int))];
        struct iovec iov = { .iov_base = "x", .iov_len = 1 };
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
        struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &pf[0], sizeof(int));
        if (sendmsg(c, &m, 0) != 1) _exit(2);
        close(pf[0]);
        close(pf[1]);
        read(c, buf, 1);                                   /* esperar a que el padre termine */
        _exit(0);
    }
    int cs = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
    check(cs >= 0, "accept4");
    struct ucred cr;
    socklen_t crl = sizeof cr;
    check(getsockopt(cs, SOL_SOCKET, SO_PEERCRED, &cr, &crl) == 0 && cr.pid == pid, "SO_PEERCRED da el pid del cliente");
    char cbuf[CMSG_SPACE(sizeof(int) * 4)];
    char d;
    struct iovec iov = { .iov_base = &d, .iov_len = 1 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    ssize_t rn = recvmsg(cs, &m, MSG_CMSG_CLOEXEC);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    int got = -1;
    if (rn == 1 && cm && cm->cmsg_type == SCM_RIGHTS) memcpy(&got, CMSG_DATA(cm), sizeof(int));
    check(got >= 0, "recvmsg recibe un descriptor (SCM_RIGHTS)");
    memset(buf, 0, sizeof buf);
    check(got >= 0 && read(got, buf, 9) == 9 && !memcmp(buf, "por el fd", 9), "el descriptor recibido es la tubería del hijo");
    write(cs, "y", 1);
    int status;
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "el cliente terminó bien");
    close(cs);
    close(ls);
    check(unlink(path) == 0 && stat(path, &st) < 0, "unlink quita el nombre del socket");

    /* Memoria compartida: el hijo escribe en un memfd mapeado, el padre lo ve */
    printf("-- memoria compartida --\n");
    int mfd = memfd_create("prueba", MFD_CLOEXEC);
    check(mfd >= 0, "memfd_create");
    check(ftruncate(mfd, 3 * 4096) == 0 && fstat(mfd, &st) == 0 && st.st_size == 3 * 4096, "ftruncate + fstat (12 KiB)");
    char *shm = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    check(shm != MAP_FAILED, "mmap MAP_SHARED");
    check(shm != MAP_FAILED && shm[0] == 0 && shm[3 * 4096 - 1] == 0, "las páginas nuevas vienen en ceros");
    pid = fork();
    if (pid == 0) {
        strcpy(shm + 4096, "escrito por el hijo");
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    check(!strcmp(shm + 4096, "escrito por el hijo"), "fork comparte las páginas (MAP_SHARED)");
    /* Otro mapeo del mismo memfd por un descriptor recibido: lo que hace un compositor Wayland */
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    pid = fork();
    if (pid == 0) {
        int nfd = -1;
        char cb[CMSG_SPACE(sizeof(int))];
        struct iovec io = { .iov_base = &d, .iov_len = 1 };
        struct msghdr mm = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
        if (recvmsg(sp[1], &mm, 0) != 1) _exit(1);
        memcpy(&nfd, CMSG_DATA(CMSG_FIRSTHDR(&mm)), sizeof(int));
        char *m2 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, nfd, 2 * 4096);
        if (m2 == MAP_FAILED) _exit(2);
        strcpy(m2, "pixeles del compositor");
        munmap(m2, 4096);
        _exit(0);
    }
    {
        char cb[CMSG_SPACE(sizeof(int))];
        struct iovec io = { .iov_base = "b", .iov_len = 1 };
        struct msghdr mm = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
        struct cmsghdr *c2 = CMSG_FIRSTHDR(&mm);
        c2->cmsg_level = SOL_SOCKET;
        c2->cmsg_type = SCM_RIGHTS;
        c2->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c2), &mfd, sizeof(int));
        sendmsg(sp[0], &mm, 0);
    }
    waitpid(pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0 && !strcmp(shm + 2 * 4096, "pixeles del compositor"),
          "otro proceso mapea el memfd recibido por SCM_RIGHTS y escribe en él");
    char rb[32] = { 0 };
    check(pread(mfd, rb, 19, 4096) == 19 && !strcmp(rb, "escrito por el hijo"), "pread del memfd ve lo mismo");
    munmap(shm, 3 * 4096);
    close(mfd);
    close(sp[0]);
    close(sp[1]);

    /* mmap de un archivo: copia privada de su contenido */
    int ff = open("/etc/hosts", O_RDONLY);
    char *fm = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, ff, 0);
    check(fm != MAP_FAILED && !strncmp(fm, "127.0.0.1", 9), "mmap de un archivo (/etc/hosts)");
    close(ff);

    printf(fallos ? "== %d fallos ==\n" : "== todo bien ==\n", fallos);
    return fallos != 0;
}
