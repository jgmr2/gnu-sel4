/*
 * sel4-agent: corre dentro de la VM Linux y ejecuta los comandos que pide
 * GNU/seL4 por el puente (/dev/hvc1), como el "interop" de WSL.
 * Protocolo: include/sel4posix/bridge.h.
 *
 * Por cada BR_OPEN lanza el comando: con una pty si se pidió terminal
 * (interactivo: docker run -it, sh...) o con tuberías si no. Reenvía la
 * salida como BR_STDOUT/BR_STDERR y, al terminar, BR_EXIT con el código.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "sel4posix/bridge.h"

#define NSESS 64

static struct sess {
    int used, tty;
    pid_t pid;
    int in, out, err;          /* in: hacia el comando; out/err: desde el comando (-1 = cerrado) */
    int exited, code;
    int mount;                 /* sesión de montaje 9p (@9p-mount): vive mientras dure el montaje */
    char mnt[256];
} sess[NSESS];

static int chan;
static int sigpipe_fds[2];

static void write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        p += n;
        len -= n;
    }
}

static void send_frame(int type, int session, const void *data, uint32_t len)
{
    struct br_frame f = { .type = type, .session = session, .len = len };
    char buf[sizeof f + BR_MAX_DATA];
    memcpy(buf, &f, sizeof f);
    if (len) memcpy(buf + sizeof f, data, len);
    write_all(chan, buf, sizeof f + len);
}

static void on_sigchld(int s)
{
    (void)s;
    char c = 0;
    int e = errno;
    write(sigpipe_fds[1], &c, 1);
    errno = e;
}

/* mkdir -p */
static void mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void spawn(int id, const char *data, uint32_t len)
{
    struct sess *s = &sess[id];
    struct br_open op;
    if (len < sizeof op) return;
    memcpy(&op, data, sizeof op);
    char *argv[256];
    uint32_t argc = 0;
    const char *p = data + sizeof op, *end = data + len;
    while (argc < op.argc && argc < 255 && p < end) {
        argv[argc++] = (char *)p;
        p += strnlen(p, end - p) + 1;
    }
    argv[argc] = NULL;
    if (!argc) {
        int32_t code = 127;
        send_frame(BR_EXIT, id, &code, 4);
        return;
    }

    memset(s, 0, sizeof *s);
    s->used = 1;
    s->tty = op.flags & BR_OPEN_TTY;
    s->in = s->out = s->err = -1;

    /* Comando interno: montar el sistema de archivos de GNU/seL4 por 9p. El
     * kernel habla 9P por un extremo de un par de sockets (trans=fd) y el otro
     * extremo se reenvía por esta sesión hasta 9pd. */
    if (!strcmp(argv[0], "@9p-mount") && argc == 2) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) {
            int32_t code = 126;
            send_frame(BR_EXIT, id, &code, 4);
            s->used = 0;
            return;
        }
        s->mount = 1;
        snprintf(s->mnt, sizeof s->mnt, "%s", argv[1]);
        pid_t pid = fork();
        if (pid == 0) {
            char opts[160];
            int fd = dup(sv[1]);                    /* sin CLOEXEC: el kernel lo usará */
            mkdir_p(s->mnt);
            snprintf(opts, sizeof opts, "trans=fd,rfdno=%d,wfdno=%d,version=9p2000.L,msize=32768,access=any", fd, fd);
            if (mount("sel4", s->mnt, "9p", 0, opts)) {
                fprintf(stderr, "sel4-agent: mount 9p en %s: %s\n", s->mnt, strerror(errno));
                _exit(1);
            }
            _exit(0);
        }
        close(sv[1]);
        s->pid = pid;
        s->in = s->out = sv[0];
        return;
    }

    int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 }, master = -1;
    char slave_name[64];
    if (s->tty) {
        master = posix_openpt(O_RDWR | O_NOCTTY);
        if (master < 0 || grantpt(master) || unlockpt(master) || ptsname_r(master, slave_name, sizeof slave_name)) {
            int32_t code = 126;
            send_frame(BR_EXIT, id, &code, 4);
            s->used = 0;
            return;
        }
        struct winsize ws = { .ws_row = op.rows ? op.rows : 24, .ws_col = op.cols ? op.cols : 80 };
        ioctl(master, TIOCSWINSZ, &ws);
    } else if (pipe(in) || pipe(out) || pipe(err)) {
        int32_t code = 126;
        send_frame(BR_EXIT, id, &code, 4);
        s->used = 0;
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGCHLD, SIG_DFL);
        setsid();                                   /* grupo propio: las señales van a todo el comando */
        if (s->tty) {
            int sl = open(slave_name, O_RDWR);
            ioctl(sl, TIOCSCTTY, 0);
            dup2(sl, 0); dup2(sl, 1); dup2(sl, 2);
            if (sl > 2) close(sl);
            close(master);
        } else {
            dup2(in[0], 0); dup2(out[1], 1); dup2(err[1], 2);
            close(in[0]); close(in[1]); close(out[0]); close(out[1]); close(err[0]); close(err[1]);
        }
        close(chan);
        setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
        setenv("HOME", "/root", 1);
        setenv("TERM", "xterm", 1);
        chdir("/root");
        execvp(argv[0], argv);
        fprintf(stderr, "linux: %s: %s\n", argv[0], strerror(errno));
        _exit(errno == ENOENT ? 127 : 126);
    }
    s->pid = pid;
    if (s->tty) {
        s->in = s->out = master;
    } else {
        close(in[0]); close(out[1]); close(err[1]);
        s->in = in[1]; s->out = out[0]; s->err = err[0];
    }
}

static void close_sess(int id)
{
    struct sess *s = &sess[id];
    if (s->in >= 0 && s->in != s->out) close(s->in);
    if (s->out >= 0) close(s->out);
    if (s->err >= 0) close(s->err);
    s->used = 0;
}

static void handle_frame(int type, int id, const char *data, uint32_t len)
{
    if (id >= NSESS) return;
    struct sess *s = &sess[id];
    switch (type) {
    case BR_OPEN:
        if (s->used) close_sess(id);
        spawn(id, data, len);
        break;
    case BR_STDIN:
        if (s->used && s->in >= 0) write_all(s->in, data, len);
        break;
    case BR_STDIN_EOF:
        if (!s->used) break;
        if (s->tty) write_all(s->in, "\004", 1);            /* Ctrl-D en la pty */
        else if (s->in >= 0) { close(s->in); s->in = -1; }
        break;
    case BR_SIGNAL:
        if (s->used && !s->exited && len >= 4) {
            int32_t sig;
            memcpy(&sig, data, 4);
            kill(-s->pid, sig);
        }
        break;
    case BR_WINSIZE:
        if (s->used && s->tty && len >= 4) {
            uint16_t rc[2];
            memcpy(rc, data, 4);
            struct winsize ws = { .ws_row = rc[0], .ws_col = rc[1] };
            ioctl(s->out, TIOCSWINSZ, &ws);
        }
        break;
    case BR_CLOSE:
        if (s->used) {
            if (s->mount) umount2(s->mnt, MNT_DETACH);
            else if (!s->exited) kill(-s->pid, SIGHUP);
            close_sess(id);
        }
        break;
    }
}

/* Reenvía lo que haya en un descriptor de salida; -1 si se cerró */
static int forward(int id, int fd, int type)
{
    char buf[16384];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
        send_frame(type, id, buf, n);
        return 0;
    }
    if (n < 0 && errno == EINTR) return 0;
    return -1;                                      /* EOF, o EIO de la pty al terminar */
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/hvc1";
    chan = open(dev, O_RDWR | O_NOCTTY);
    if (chan < 0) { perror(dev); return 1; }
    struct termios t;
    if (!tcgetattr(chan, &t)) {                     /* canal de bytes: sin traducciones */
        cfmakeraw(&t);
        tcsetattr(chan, TCSANOW, &t);
    }
    pipe2(sigpipe_fds, O_CLOEXEC | O_NONBLOCK);
    signal(SIGCHLD, on_sigchld);
    signal(SIGPIPE, SIG_IGN);
    send_frame(BR_HELLO, 0, NULL, 0);

    static char rbuf[2 * (sizeof(struct br_frame) + BR_MAX_DATA)];
    size_t rlen = 0;
    for (;;) {
        struct pollfd pf[2 + 2 * NSESS];
        int map[2 + 2 * NSESS], kind[2 + 2 * NSESS], n = 0;
        pf[n].fd = chan; pf[n].events = POLLIN; n++;
        pf[n].fd = sigpipe_fds[0]; pf[n].events = POLLIN; n++;
        for (int i = 0; i < NSESS; i++) {
            if (!sess[i].used) continue;
            if (sess[i].out >= 0) { pf[n].fd = sess[i].out; pf[n].events = POLLIN; map[n] = i; kind[n] = BR_STDOUT; n++; }
            if (sess[i].err >= 0) { pf[n].fd = sess[i].err; pf[n].events = POLLIN; map[n] = i; kind[n] = BR_STDERR; n++; }
        }
        if (poll(pf, n, -1) < 0 && errno != EINTR) break;

        /* Salidas de los comandos */
        for (int k = 2; k < n; k++) {
            if (!pf[k].revents) continue;
            struct sess *s = &sess[map[k]];
            if (forward(map[k], pf[k].fd, kind[k]) < 0) {
                if (kind[k] == BR_STDOUT) {
                    if (s->in == s->out) s->in = -1;
                    close(s->out); s->out = -1;
                } else {
                    close(s->err); s->err = -1;
                }
            }
        }

        /* Hijos que terminaron */
        if (pf[1].revents) {
            char c;
            while (read(sigpipe_fds[0], &c, 1) > 0) ;
        }
        int st;
        pid_t pid;
        while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
            for (int i = 0; i < NSESS; i++) {
                if (sess[i].used && sess[i].pid == pid) {
                    sess[i].exited = 1;
                    sess[i].code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
                }
            }
        }
        for (int i = 0; i < NSESS; i++) {
            struct sess *s = &sess[i];
            if (!s->used || !s->exited) continue;
            if (s->mount) {
                /* el ayudante de mount terminó: si falló, se informa; si no, la sesión sigue */
                if (s->code) {
                    int32_t code = s->code;
                    send_frame(BR_EXIT, i, &code, 4);
                    close_sess(i);
                } else {
                    s->exited = 0;
                }
                continue;
            }
            /* con pty, al morir el comando se drena lo que quede y se cierra */
            if (s->tty && s->out >= 0) {
                while (forward(i, s->out, BR_STDOUT) == 0) {
                    struct pollfd q = { .fd = s->out, .events = POLLIN };
                    if (poll(&q, 1, 0) <= 0) break;
                }
                close(s->out); s->out = s->in = -1;
            }
            if (s->out < 0 && s->err < 0) {
                int32_t code = s->code;
                send_frame(BR_EXIT, i, &code, 4);
                close_sess(i);
            }
        }

        /* Tramas desde GNU/seL4 */
        if (pf[0].revents & POLLIN) {
            ssize_t r = read(chan, rbuf + rlen, sizeof rbuf - rlen);
            if (r > 0) rlen += r;
            size_t off = 0;
            while (rlen - off >= sizeof(struct br_frame)) {
                struct br_frame f;
                memcpy(&f, rbuf + off, sizeof f);
                if (f.len > BR_MAX_DATA) { off = rlen; break; }     /* corrupta: descartar */
                if (rlen - off < sizeof f + f.len) break;
                handle_frame(f.type, f.session, rbuf + off + sizeof f, f.len);
                off += sizeof f + f.len;
            }
            memmove(rbuf, rbuf + off, rlen - off);
            rlen -= off;
        }
    }
    return 0;
}
