/*
 * VFS de posix_server: descriptores de archivo, directorio actual, rutas y
 * despacho de las llamadas de archivos al servidor de sistema de archivos
 * (fs_ext2, por el protocolo de include/sel4posix/fs_protocol.h).
 *
 * Como en Unix, un descriptor apunta a una "descripción de archivo abierto"
 * (struct ofile) que guarda el desplazamiento; dup() comparte la misma.
 *
 * Cada proceso tiene su tabla de descriptores, directorio actual y umask
 * (struct proc); las descripciones abiertas y las tuberías son globales, así
 * que fork() las comparte entre padre e hijo.
 *
 * Por ahora hay un único sistema de archivos montado en "/" y las rutas se
 * normalizan de forma léxica ("a/../b" -> "b") antes de enviarlas.
 */
#include <os/sddf.h>
#include <string.h>
#include <sel4posix/fs_protocol.h>
#include <sel4posix/net_protocol.h>
#include <sddf/util/printf.h>
#include "server.h"
#include "syscall_nr.h"

/* Búfer compartido con el servidor de archivos (lo fija Microkit, ver meta.py) */
uintptr_t fs_buf;

#define AT_FDCWD             (-100)
#define AT_SYMLINK_NOFOLLOW  0x100
#define AT_REMOVEDIR         0x200
#define AT_EMPTY_PATH        0x1000

#define O_ACCMODE    03
#define O_RDONLY     00
#define O_WRONLY     01
#define O_CREAT      0100
#define O_EXCL       0200
#define O_TRUNC      01000
#define O_APPEND     02000
#define O_NONBLOCK   04000
#define O_DIRECTORY  0200000
#define O_NOFOLLOW   0400000
#define O_CLOEXEC    02000000

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFLNK  0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

#define F_DUPFD          0
#define F_GETFD          1
#define F_SETFD          2
#define F_GETFL          3
#define F_SETFL          4
#define F_GETLK          5
#define F_SETLK          6
#define F_SETLKW         7
#define F_DUPFD_CLOEXEC  1030
#define FD_CLOEXEC       1

#define X_OK 1

#define K_STAT_SIZE    144     /* struct stat de Linux x86_64 */
#define K_STATFS_SIZE  120     /* struct statfs de Linux x86_64 */
#define ST_SIZE_OFFSET 48      /* offsetof(struct stat, st_size) */

/* ---------------- descriptores ---------------- */

enum { OF_FREE, OF_TTY, OF_NULL, OF_FILE, OF_DIR, OF_PIPE_R, OF_PIPE_W, OF_LINUX, OF_SOCKET,
       OF_PROCDIR, OF_PROCFILE,        /* /proc (procfs.c): se identifica por la ruta */
       OF_EVENTFD, OF_TIMERFD,         /* contadores y temporizadores que se leen como archivos */
       OF_EPOLL,                       /* marcador: el conjunto de interés vive en el runtime */
       OF_USOCK,                       /* socket local (AF_UNIX) */
       OF_SHM,                         /* memfd: memoria compartida (mem.c) */
       OF_KMSG,                        /* /dev/kmsg: al puerto serie (registros, make test) */
       OF_RANDOM };                    /* /dev/urandom, /dev/random (RDRAND) */

/* ¿La ruta es de /proc? */
static bool is_proc(const char *abs)
{
    return !strncmp(abs, "/proc", 5) && (abs[5] == 0 || abs[5] == '/');
}

struct ofile {
    int refs;
    int type;
    uint32_t ino;               /* archivo/directorio; índice de la tubería; socket de netstack */
    uint64_t off;               /* (socket: 1 = connect() bloqueante en curso) */
    uint32_t flags;
    char path[FS_PATH_MAX];     /* ruta absoluta (para fchdir y las llamadas *at) */
};

#define NOFILE 128
static struct ofile ofiles[NOFILE];

/* Tuberías: búfer circular; quien bloquea queda en rwait/wwait (bits = índice de proceso) */
#define NPIPE     32
#define PIPE_SIZE 16384
static struct pipe {
    bool used;
    int readers, writers;
    uint32_t head, tail;        /* tail - head = bytes en el búfer */
    uint32_t rwait, wwait;
    char buf[PIPE_SIZE];
} pipes[NPIPE];

static long net_call(long op, long a1, long a2, long a3, long a4);

/* Sockets locales (AF_UNIX): implementados más abajo, junto a poll */
#define AF_UNIX_      1
#define S_IFSOCK_     0140000
static void usock_free(int i);
static long usock_new(long type);
static bool usock_bound(const char *abs, bool remove);

/* eventfd: contador; read lo vacía (o resta 1 con EFD_SEMAPHORE), write suma */
#define NEVFD 32
static struct evfd {
    bool used, semaphore;
    uint64_t count;
    uint32_t rwait, wwait;
} evfds[NEVFD];

/* timerfd: plazos en ns del temporizador del servidor; read da las expiraciones */
#define NTFD 32
static struct tfd {
    bool used;
    uint64_t next, interval, expirations;
    uint32_t rwait;
} tfds[NTFD];

static struct ofile *fd_get(long fd)
{
    if (fd < 0 || fd >= NFD || cur->fdt[fd].of < 0) return NULL;
    return &ofiles[cur->fdt[fd].of];
}

static int ofile_alloc(int type)
{
    for (int i = 0; i < NOFILE; i++) {
        if (!ofiles[i].refs) {
            ofiles[i].refs = 1;
            ofiles[i].type = type;
            ofiles[i].ino = 0;
            ofiles[i].off = 0;
            ofiles[i].flags = 0;
            ofiles[i].path[0] = 0;
            return i;
        }
    }
    return -1;
}

static int fd_alloc(int of, long min, bool cloexec)
{
    for (long fd = min < 0 ? 0 : min; fd < NFD; fd++) {
        if (cur->fdt[fd].of < 0) {
            cur->fdt[fd].of = of;
            cur->fdt[fd].cloexec = cloexec;
            return fd;
        }
    }
    return -EMFILE;
}

/* Suelta una referencia a una descripción abierta; al liberarla, las tuberías
 * despiertan al otro extremo (EOF para lectores, EPIPE para escritores) */
static void ofile_release(int of)
{
    struct ofile *f = &ofiles[of];
    if (--f->refs > 0) return;
    if (f->type == OF_PIPE_R || f->type == OF_PIPE_W) {
        struct pipe *pp = &pipes[f->ino];
        if (f->type == OF_PIPE_R) pp->readers--; else pp->writers--;
        proc_wake_mask(pp->rwait | pp->wwait);
        pp->rwait = pp->wwait = 0;
        if (!pp->readers && !pp->writers) pp->used = false;
    }
    if (f->type == OF_LINUX) bridge_close(f->ino);
    if (f->type == OF_SOCKET) net_call(NET_OP_CLOSE, f->ino, 0, 0, 0);
    if (f->type == OF_EVENTFD) evfds[f->ino].used = false;
    if (f->type == OF_TIMERFD) { tfds[f->ino].used = false; timer_rearm(); }
    int type = f->type;
    f->type = OF_FREE;
    if (type == OF_USOCK) usock_free(f->ino);           /* puede soltar descriptores en tránsito */
    if (type == OF_SHM) mem_shm_unref(f->ino);
}

static long fd_close(long fd)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    ofile_release(cur->fdt[fd].of);
    cur->fdt[fd].of = -1;
    return 0;
}

/* ---------------- servidor de archivos ---------------- */

static long fs_call(long op, uint64_t a1, uint64_t a2, uint64_t a3)
{
    microkit_mr_set(0, op);
    microkit_mr_set(1, a1);
    microkit_mr_set(2, a2);
    microkit_mr_set(3, a3);
    microkit_ppcall(FS_CH, microkit_msginfo_new(0, 4));
    return (long)microkit_mr_get(0);
}

static void fs_set_path(uint32_t off, const char *abs)
{
    size_t n = strlen(abs);
    memcpy((char *)fs_buf + off, abs, n + 1);
}

/* Busca una ruta absoluta. Devuelve el inodo y, opcionalmente, modo y tamaño. */
static long fs_lookup(const char *abs, bool follow, uint32_t *mode, uint64_t *size)
{
    fs_set_path(0, abs);
    long ino = fs_call(FS_OP_LOOKUP, follow, 0, 0);
    if (ino >= 0) {
        if (mode) *mode = microkit_mr_get(1);
        if (size) *size = microkit_mr_get(2);
    }
    return ino;
}

static long fs_size(uint32_t ino, uint64_t *size)
{
    long r = fs_call(FS_OP_STAT, ino, 0, 0);
    if (r >= 0) memcpy(size, (char *)fs_buf + ST_SIZE_OFFSET, 8);
    return r;
}

/* ---------------- rutas ---------------- */

static const char *client_path(uint32_t off)
{
    char *p = (char *)client_buf + off;
    p[FS_PATH_MAX - 1] = 0;
    return p;
}

/* Convierte path (relativa a dirfd o al directorio actual) en ruta absoluta normalizada */
static long normalize(long dirfd, const char *path, char *out)
{
    static char tmp[FS_PATH_MAX * 2];
    if (!*path) return -ENOENT;
    const char *base = "";
    if (path[0] != '/') {
        if (dirfd == AT_FDCWD) {
            base = cur->cwd;
        } else {
            struct ofile *f = fd_get(dirfd);
            if (!f) return -EBADF;
            if (f->type != OF_DIR && f->type != OF_PROCDIR) return -ENOTDIR;
            base = f->path;
        }
    }
    size_t bl = strlen(base), pl = strlen(path);
    if (bl + pl + 2 > sizeof(tmp)) return -ENAMETOOLONG;
    memcpy(tmp, base, bl);
    tmp[bl] = '/';
    memcpy(tmp + bl + 1, path, pl + 1);

    size_t o = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = e - p;
        if (len == 1 && p[0] == '.') {
            /* nada */
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;    /* quita el último componente */
            if (o > 0) o--;
        } else {
            if (o + 1 + len + 1 > FS_PATH_MAX) return -ENAMETOOLONG;
            out[o++] = '/';
            memcpy(out + o, p, len);
            o += len;
        }
        p = e;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    return 0;
}

/* Dispositivos que viven en el servidor y no en el disco */
static int dev_type(const char *abs)
{
    if (!strcmp(abs, "/dev/tty") || !strcmp(abs, "/dev/console")) return OF_TTY;
    if (!strcmp(abs, "/dev/null")) return OF_NULL;
    if (!strcmp(abs, "/dev/linux")) return OF_LINUX;   /* sesión con la VM Linux */
    if (!strcmp(abs, "/dev/kmsg")) return OF_KMSG;     /* mensajes al puerto serie */
    if (!strcmp(abs, "/dev/urandom") || !strcmp(abs, "/dev/random")) return OF_RANDOM;
    return OF_FREE;
}

/* struct stat para los dispositivos de caracteres del servidor */
static void dev_stat(int type)
{
    uint64_t *st = (uint64_t *)client_buf;
    memset(st, 0, K_STAT_SIZE);
    st[0] = 5;                                                 /* st_dev */
    st[1] = type == OF_TTY ? 1 : 2;                            /* st_ino */
    st[2] = 1;                                                 /* st_nlink */
    ((uint32_t *)client_buf)[6] = S_IFCHR | (type == OF_TTY ? 0620 : 0666);   /* st_mode */
    st[5] = type == OF_TTY ? 0x0500 : 0x0103;                  /* st_rdev */
    st[7] = 1024;                                              /* st_blksize */
}

/* ---------------- llamadas ---------------- */

static long do_open(long dirfd, uint32_t flags, uint32_t mode)
{
    char abs[FS_PATH_MAX];
    long r = normalize(dirfd, client_path(0), abs);
    if (r) return r;

    int type = dev_type(abs);
    uint32_t ino = 0;
    if (type == OF_FREE && is_proc(abs)) {
        int k = procfs_kind(abs);
        if (k == PROCFS_LINK) {                       /* /proc/self: abrir /proc/<pid> */
            char t[32];
            long n = procfs_readlink(abs, t, sizeof t - 1);
            if (n < 0 || t[0] == '/') return -ENOENT;
            t[n] = 0;
            memcpy(abs, "/proc/", 6);
            memcpy(abs + 6, t, n + 1);
            k = procfs_kind(abs);
        }
        if (k == PROCFS_NONE) return (flags & O_CREAT) ? -EACCES : -ENOENT;
        if ((flags & O_ACCMODE) != O_RDONLY) return k == PROCFS_DIR ? -EISDIR : -EACCES;
        if (k == PROCFS_FILE && (flags & O_DIRECTORY)) return -ENOTDIR;
        type = k == PROCFS_DIR ? OF_PROCDIR : OF_PROCFILE;
    }
    if (type == OF_FREE) {
        uint32_t fmode;
        uint64_t size;
        bool created = false;
        long res = fs_lookup(abs, !(flags & O_NOFOLLOW), &fmode, &size);
        if (res == -ENOENT && (flags & O_CREAT)) {
            fs_set_path(0, abs);
            res = fs_call(FS_OP_CREATE, S_IFREG | (mode & 07777 & ~cur->umask), 0, 0);
            fmode = S_IFREG;
            size = 0;
            created = true;
        } else if (res >= 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
            return -EEXIST;
        }
        if (res < 0) return res;
        ino = res;
        if (S_ISLNK(fmode)) return -ELOOP;
        if (S_ISDIR(fmode)) {
            if ((flags & O_ACCMODE) != O_RDONLY) return -EISDIR;
            type = OF_DIR;
        } else {
            if (flags & O_DIRECTORY) return -ENOTDIR;
            type = OF_FILE;
            if ((flags & O_TRUNC) && S_ISREG(fmode) && (flags & O_ACCMODE) != O_RDONLY && !created && size) {
                res = fs_call(FS_OP_TRUNCATE, ino, 0, 0);
                if (res < 0) return res;
            }
        }
    }
    if (type == OF_LINUX) {
        long sid = bridge_open();
        if (sid < 0) return sid;
        ino = sid;
    }
    int of = ofile_alloc(type);
    if (of < 0) {
        if (type == OF_LINUX) bridge_close(ino);
        return -EMFILE;
    }
    ofiles[of].ino = ino;
    ofiles[of].flags = flags;
    memcpy(ofiles[of].path, abs, strlen(abs) + 1);
    int fd = fd_alloc(of, 0, flags & O_CLOEXEC);
    if (fd < 0) ofiles[of].refs = 0;
    return fd;
}

static long pipe_read(struct ofile *f, size_t len)
{
    struct pipe *pp = &pipes[f->ino];
    uint32_t avail = pp->tail - pp->head;
    if (!avail) {
        if (!pp->writers) return 0;                       /* fin de archivo */
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        pp->rwait |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    size_t n = len < avail ? len : avail;
    if (n > POSIX_BUF_SIZE) n = POSIX_BUF_SIZE;
    for (size_t i = 0; i < n; i++) ((char *)client_buf)[i] = pp->buf[(pp->head + i) % PIPE_SIZE];
    pp->head += n;
    proc_wake_mask(pp->wwait);
    pp->wwait = 0;
    return n;
}

static long pipe_write(struct ofile *f, size_t len)
{
    struct pipe *pp = &pipes[f->ino];
    if (!pp->readers) {
        proc_signal(cur, SIGPIPE);
        return -EPIPE;
    }
    uint32_t space = PIPE_SIZE - (pp->tail - pp->head);
    if (!space) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        pp->wwait |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    size_t n = len < space ? len : space;
    for (size_t i = 0; i < n; i++) pp->buf[(pp->tail + i) % PIPE_SIZE] = ((const char *)client_buf)[i];
    pp->tail += n;
    proc_wake_mask(pp->rwait);
    pp->rwait = 0;
    return n;
}

static long do_pipe(long flags)
{
    int pi = -1;
    for (int i = 0; i < NPIPE; i++) if (!pipes[i].used) { pi = i; break; }
    if (pi < 0) return -EMFILE;
    int r = ofile_alloc(OF_PIPE_R), w = r >= 0 ? ofile_alloc(OF_PIPE_W) : -1;
    if (r < 0 || w < 0) {
        if (r >= 0) ofiles[r].refs = 0;
        return -EMFILE;
    }
    int fr = fd_alloc(r, 0, flags & O_CLOEXEC);
    int fw = fr >= 0 ? fd_alloc(w, 0, flags & O_CLOEXEC) : -EMFILE;
    if (fw < 0) {
        if (fr >= 0) cur->fdt[fr].of = -1;
        ofiles[r].refs = ofiles[w].refs = 0;
        return -EMFILE;
    }
    struct pipe *pp = &pipes[pi];
    pp->used = true;
    pp->readers = pp->writers = 1;
    pp->head = pp->tail = 0;
    pp->rwait = pp->wwait = 0;
    ofiles[r].ino = ofiles[w].ino = pi;
    ofiles[r].flags = flags & O_NONBLOCK;
    ofiles[w].flags = 1 | (flags & O_NONBLOCK);       /* O_WRONLY */
    microkit_mr_set(1, fr);
    microkit_mr_set(2, fw);
    return 0;
}

/* ---------------- eventfd y timerfd ---------------- */

#define EFD_SEMAPHORE 1
#define EVFD_MAX 0xfffffffffffffffeULL

static long evfd_new(uint64_t init, long flags)
{
    int i = 0;
    while (i < NEVFD && evfds[i].used) i++;
    if (i == NEVFD) return -ENFILE;
    int of = ofile_alloc(OF_EVENTFD);
    if (of < 0) return -ENFILE;
    evfds[i] = (struct evfd){ .used = true, .semaphore = flags & EFD_SEMAPHORE, .count = init };
    ofiles[of].ino = i;
    ofiles[of].flags = 2 | (flags & O_NONBLOCK);
    int fd = fd_alloc(of, 0, flags & O_CLOEXEC);
    if (fd < 0) ofile_release(of);
    return fd;
}

static long evfd_read(struct ofile *f, size_t len)
{
    struct evfd *e = &evfds[f->ino];
    if (len < 8) return -EINVAL;
    if (!e->count) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        e->rwait |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    uint64_t v = e->semaphore ? 1 : e->count;
    e->count -= v;
    memcpy((void *)client_buf, &v, 8);
    proc_wake_mask(e->wwait);
    e->wwait = 0;
    return 8;
}

static long evfd_write(struct ofile *f, size_t len)
{
    struct evfd *e = &evfds[f->ino];
    uint64_t v;
    if (len < 8) return -EINVAL;
    memcpy(&v, (void *)client_buf, 8);
    if (v == ~0ULL) return -EINVAL;
    if (v > EVFD_MAX - e->count) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        e->wwait |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    e->count += v;
    proc_wake_mask(e->rwait);
    e->rwait = 0;
    return 8;
}

static long tfd_new(long flags)
{
    int i = 0;
    while (i < NTFD && tfds[i].used) i++;
    if (i == NTFD) return -ENFILE;
    int of = ofile_alloc(OF_TIMERFD);
    if (of < 0) return -ENFILE;
    tfds[i] = (struct tfd){ .used = true };
    ofiles[of].ino = i;
    ofiles[of].flags = flags & O_NONBLOCK;
    int fd = fd_alloc(of, 0, flags & O_CLOEXEC);
    if (fd < 0) ofile_release(of);
    return fd;
}

/* Acumula las expiraciones vencidas */
static void tfd_update(struct tfd *t, uint64_t now)
{
    if (!t->next || now < t->next) return;
    uint64_t n = 1 + (t->interval ? (now - t->next) / t->interval : 0);
    t->expirations += n;
    t->next = t->interval ? t->next + n * t->interval : 0;
}

/* settime: valor (relativo, ns; 0 = desarmar) e intervalo; los anteriores en MR1/MR2 */
static long tfd_settime(struct ofile *f, bool set, uint64_t value, uint64_t interval)
{
    struct tfd *t = &tfds[f->ino];
    uint64_t now = timer_now_ns();
    tfd_update(t, now);
    microkit_mr_set(1, t->next ? t->next - now : 0);
    microkit_mr_set(2, t->interval);
    if (set) {
        t->next = value ? now + value : 0;
        t->interval = value ? interval : 0;
        t->expirations = 0;
        timer_rearm();
    }
    return 0;
}

static long tfd_read(struct ofile *f, size_t len)
{
    struct tfd *t = &tfds[f->ino];
    if (len < 8) return -EINVAL;
    tfd_update(t, timer_now_ns());
    if (!t->expirations) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        t->rwait |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    memcpy((void *)client_buf, &t->expirations, 8);
    t->expirations = 0;
    return 8;
}

/* Para clock.c: el plazo más cercano de los timerfd, y despertar a quien espera los vencidos */
uint64_t vfs_timerfd_next(void)
{
    uint64_t next = 0;
    for (int i = 0; i < NTFD; i++)
        if (tfds[i].used && tfds[i].next && (!next || tfds[i].next < next)) next = tfds[i].next;
    return next;
}

void vfs_timerfd_fire(uint64_t now)
{
    for (int i = 0; i < NTFD; i++) {
        struct tfd *t = &tfds[i];
        if (!t->used || !t->next || now < t->next) continue;
        tfd_update(t, now);
        proc_wake_mask(t->rwait);
        t->rwait = 0;
    }
}

/* ---------------- sockets (pila TCP/IP de FreeBSD en netstack) ---------------- */

/* Dentro de netstack los sockets nunca bloquean: aquí se decide si la llamada
 * espera (POSIX_RETRY, se reintenta cuando netstack avisa de actividad) */
uintptr_t net_buf;                         /* compartido con netstack (setvar en meta.py) */
static uint32_t net_waiters;

#define FIONREAD     0x541B
#define FIONBIO      0x5421
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC  02000000
#define MSG_NOSIGNAL 0x4000
#define MSG_DONTWAIT 0x40
#define SOL_SOCKET   1
#define SO_ERROR     4
#define EINPROGRESS  115
#define EALREADY     114
#define EISCONN      106
#define ENOTSOCK     88
#define EOPNOTSUPP   95
#define POLLOUT_     0x004
#define POLLERR_     0x008
#define POLLHUP_     0x010

static long net_call(long op, long a1, long a2, long a3, long a4)
{
    microkit_mr_set(0, op);
    microkit_mr_set(1, a1);
    microkit_mr_set(2, a2);
    microkit_mr_set(3, a3);
    microkit_mr_set(4, a4);
    microkit_ppcall(NET_CH, microkit_msginfo_new(0, 5));
    return (long)microkit_mr_get(0);
}

/* netstack avisa: lo que esperaba un socket se reintenta */
void net_notified(void)
{
    proc_wake_mask(net_waiters);
    net_waiters = 0;
}

/* ---------------- configuración de red (DHCP de netstack) ---------------- */

static const char *ip4(uint32_t a, char *out)       /* orden de red -> "a.b.c.d" */
{
    const uint8_t *b = (const uint8_t *)&a;
    sddf_snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return out;
}

static bool net_info(struct net_info *ni)
{
    if (net_call(NET_OP_INFO, 0, 0, 0, 0) < 0) return false;
    memcpy(ni, (void *)net_buf, sizeof *ni);
    return true;
}

/* Reemplaza un archivo entero desde el servidor */
static long write_file(const char *abs, const char *data, size_t len)
{
    long ino = fs_lookup(abs, true, NULL, NULL);
    if (ino == -ENOENT) {
        fs_set_path(0, abs);
        ino = fs_call(FS_OP_CREATE, S_IFREG | 0644, 0, 0);
    }
    if (ino < 0) return ino;
    long r = fs_call(FS_OP_TRUNCATE, ino, 0, 0);
    if (r < 0) return r;
    memcpy((void *)fs_buf, data, len);
    return fs_call(FS_OP_WRITE, ino, 0, len);
}

/* Concesión nueva o renovada: los DNS que dio el servidor DHCP van a /etc/resolv.conf */
void net_lease_changed(void)
{
    struct net_info ni;
    if (!net_info(&ni) || !ni.dns[0]) return;
    char buf[192], a[16], b[16];
    int n = sddf_snprintf(buf, sizeof buf, "# Lo escribe posix_server con la configuración de red (red, DHCP)\n"
                          "nameserver %s\n", ip4(ni.dns[0], a));
    if (ni.dns[1]) n += sddf_snprintf(buf + n, sizeof buf - n, "nameserver %s\n", ip4(ni.dns[1], b));
    write_file("/etc/resolv.conf", buf, n);
}

/* /proc/net/dhcp */
long net_info_text(char *out, size_t max)
{
    static const char *const states[] = { "apagado", "pidiendo la dirección anterior", "buscando servidor",
                                          "pidiendo la dirección ofrecida", "concedida", "renovando",
                                          "fija (sin DHCP)" };
    struct net_info ni;
    if (!net_info(&ni)) return sddf_snprintf(out, max, "netstack no responde\n");
    char ip[16], gw[16], d0[16], d1[16], sv[16];
    int pl = __builtin_popcount(ni.mask);
    return sddf_snprintf(out, max,
        "estado:    %s\ninterfaz:  eth0 %02x:%02x:%02x:%02x:%02x:%02x\ndirección: %s/%d\npuerta:    %s\n"
        "dns:       %s %s\nservidor:  %s\nconcesión: %u s (quedan %u s)\n",
        ni.state < 7 ? states[ni.state] : "?", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5],
        ip4(ni.ip, ip), pl, ip4(ni.gw, gw), ip4(ni.dns[0], d0), ni.dns[1] ? ip4(ni.dns[1], d1) : "",
        ip4(ni.server, sv), ni.lease, ni.remaining);
}

static long sock_wait(struct ofile *f, long r, long flags)
{
    if (r != -EAGAIN) return r;
    if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
    net_waiters |= 1u << PROC_INDEX(cur);
    return POSIX_RETRY;
}

/* La dirección que devolvió netstack -> búfer del proceso (MR1 = longitud) */
static void sock_addr_out(void)
{
    long alen = microkit_mr_get(1);
    if (alen > NET_ADDR_MAX) alen = NET_ADDR_MAX;
    memcpy((void *)(client_buf + NET_ADDR_OFF), (void *)(net_buf + NET_ADDR_OFF), alen);
    microkit_mr_set(1, alen);
}

static long sock_socket(long domain, long type, long proto)
{
    int of = ofile_alloc(OF_SOCKET);
    if (of < 0) return -ENFILE;
    if (domain == AF_UNIX_) {
        ofiles[of].refs = 0;
        ofiles[of].type = OF_FREE;
        return usock_new(type);
    }
    long s = net_call(NET_OP_SOCKET, domain, type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC), proto, 0);
    if (s < 0) {
        ofiles[of].refs = 0;
        ofiles[of].type = OF_FREE;
        return s;
    }
    ofiles[of].ino = s;
    ofiles[of].flags = 2 | (type & SOCK_NONBLOCK ? O_NONBLOCK : 0);       /* O_RDWR */
    int fd = fd_alloc(of, 0, type & SOCK_CLOEXEC);
    if (fd < 0) ofile_release(of);
    return fd;
}

/* bind y connect: dirección en el búfer del proceso, en NET_ADDR_OFF */
static long sock_addr_call(long op, struct ofile *f, long alen)
{
    if (alen < 0 || alen > NET_ADDR_MAX) return -EINVAL;
    memcpy((void *)(net_buf + NET_ADDR_OFF), (void *)(client_buf + NET_ADDR_OFF), alen);
    return net_call(op, f->ino, alen, 0, 0);
}

static long sock_connect(struct ofile *f, long alen)
{
    if (f->off) {                                     /* connect() bloqueante ya en curso */
        long p = net_call(NET_OP_POLL, f->ino, POLLOUT_, 0, 0);
        if (p >= 0 && !(p & (POLLOUT_ | POLLERR_ | POLLHUP_))) {
            net_waiters |= 1u << PROC_INDEX(cur);
            return POSIX_RETRY;
        }
        f->off = 0;
        long r = net_call(NET_OP_GETSOCKOPT, f->ino, SOL_SOCKET, SO_ERROR, 4);
        if (r < 0) return r;
        int err = *(int *)net_buf;
        return err ? -err : 0;
    }
    long r = sock_addr_call(NET_OP_CONNECT, f, alen);
    if (r != -EINPROGRESS) return r;
    if (f->flags & O_NONBLOCK) return r;
    f->off = 1;
    net_waiters |= 1u << PROC_INDEX(cur);
    return POSIX_RETRY;
}

static long sock_accept(struct ofile *f, long flags)
{
    int of = ofile_alloc(OF_SOCKET);
    if (of < 0) return -ENFILE;
    long s = net_call(NET_OP_ACCEPT, f->ino, 0, 0, 0);
    if (s < 0) {
        ofiles[of].refs = 0;
        ofiles[of].type = OF_FREE;
        return sock_wait(f, s, 0);
    }
    sock_addr_out();
    ofiles[of].ino = s;
    ofiles[of].flags = 2 | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0);
    int fd = fd_alloc(of, 0, flags & SOCK_CLOEXEC);
    if (fd < 0) ofile_release(of);
    return fd;
}

/* send/sendto/write: datos al principio del búfer, dirección (alen > 0) en NET_ADDR_OFF */
static long sock_send(struct ofile *f, size_t len, long flags, long alen)
{
    if (len > NET_DATA_MAX) len = NET_DATA_MAX;
    memcpy((void *)net_buf, (void *)client_buf, len);
    if (alen > 0) memcpy((void *)(net_buf + NET_ADDR_OFF), (void *)(client_buf + NET_ADDR_OFF), alen);
    long r = net_call(NET_OP_SEND, f->ino, len, flags, alen > 0 ? alen : 0);
    if (r == -EPIPE && !(flags & MSG_NOSIGNAL)) proc_signal(cur, SIGPIPE);
    return sock_wait(f, r, flags);
}

/* recv/recvfrom/read: datos al principio del búfer; dirección de origen en NET_ADDR_OFF (MR1) */
static long sock_recv(struct ofile *f, size_t len, long flags)
{
    if (len > NET_DATA_MAX) len = NET_DATA_MAX;
    long r = net_call(NET_OP_RECV, f->ino, len, flags, 0);
    microkit_mr_set(2, 0);                              /* sin descriptores (solo AF_UNIX) */
    if (r < 0) return sock_wait(f, r, flags);
    memcpy((void *)client_buf, (void *)net_buf, r);
    sock_addr_out();
    return r;
}

static long usock_recv(struct ofile *f, size_t len, long flags, long maxfds);
static long usock_send(struct ofile *f, size_t len, long flags, long nfds);

static long do_read(long fd, size_t len, bool positional, uint64_t pos)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (f->type) {
    case OF_TTY:    return positional ? -ESPIPE : tty_read(len);
    case OF_NULL:   return 0;
    case OF_DIR:    return -EISDIR;
    case OF_PIPE_R: return positional ? -ESPIPE : pipe_read(f, len);
    case OF_PIPE_W: return -EBADF;
    case OF_LINUX:  return bridge_read(f->ino, (uint8_t *)client_buf, len > POSIX_BUF_SIZE ? POSIX_BUF_SIZE : len);
    case OF_SOCKET: return positional ? -ESPIPE : sock_recv(f, len, 0);
    case OF_USOCK:   return positional ? -ESPIPE : usock_recv(f, len, 0, 0);
    case OF_KMSG:    return 0;
    case OF_RANDOM: {
        if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
        for (size_t i = 0; i < len; i += 8) {
            unsigned long long v = 0;
            for (int k = 0; k < 10; k++) {
                unsigned char ok;
                __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
                if (ok) break;
                v ^= clock_rdtsc() * 0x9e3779b97f4a7c15ULL;
            }
            memcpy((char *)client_buf + i, &v, len - i < 8 ? len - i : 8);
        }
        return len;
    }
    case OF_SHM: {
        if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
        long r = mem_shm_rw(f->ino, positional ? pos : f->off, (void *)client_buf, len, false);
        if (r > 0 && !positional) f->off += r;
        return r;
    }
    case OF_EVENTFD: return evfd_read(f, len);
    case OF_TIMERFD: return tfd_read(f, len);
    case OF_EPOLL:   return -EINVAL;
    case OF_PROCDIR: return -EISDIR;
    case OF_PROCFILE: {
        if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
        long r = procfs_read(f->path, positional ? pos : f->off, (char *)client_buf, len);
        if (r > 0 && !positional) f->off += r;
        return r;
    }
    }
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    long r = fs_call(FS_OP_READ, f->ino, positional ? pos : f->off, len);
    if (r > 0) {
        memcpy((void *)client_buf, (void *)fs_buf, r);
        if (!positional) f->off += r;
    }
    return r;
}

static long do_write(long fd, size_t len, bool positional, uint64_t pos)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (f->type) {
    case OF_TTY:    return positional ? -ESPIPE : tty_write((const char *)client_buf, len);
    case OF_NULL:   return len;
    case OF_DIR:    return -EISDIR;
    case OF_PIPE_W: return positional ? -ESPIPE : pipe_write(f, len);
    case OF_PIPE_R: return -EBADF;
    case OF_LINUX:  return bridge_write(f->ino, (const uint8_t *)client_buf, len);
    case OF_SOCKET: return positional ? -ESPIPE : sock_send(f, len, 0, 0);
    case OF_PROCDIR: case OF_PROCFILE: return -EBADF;
    case OF_USOCK:   return positional ? -ESPIPE : usock_send(f, len, 0, 0);
    case OF_RANDOM:  return len;                       /* escribir "entropía": se acepta */
    case OF_KMSG: {                                    /* al puerto serie (depuración del kernel) */
        static char line[257];
        size_t n = len < 256 ? len : 256;
        memcpy(line, (const void *)client_buf, n);
        line[n] = 0;
        microkit_dbg_puts(line);
        return n;
    }
    case OF_SHM: {                                     /* como un archivo: crece al escribir */
        if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
        uint64_t off = positional ? pos : f->off;
        if (off + len > mem_shm_size(f->ino)) {
            long t = mem_shm_truncate(f->ino, off + len);
            if (t < 0) return t;
        }
        long r = mem_shm_rw(f->ino, off, (void *)client_buf, len, true);
        if (r > 0 && !positional) f->off = off + r;
        return r;
    }
    case OF_EVENTFD: return evfd_write(f, len);
    case OF_TIMERFD: case OF_EPOLL: return -EINVAL;
    }
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    uint64_t off = positional ? pos : f->off;
    if ((f->flags & O_APPEND) && !positional) {
        long r = fs_size(f->ino, &off);
        if (r < 0) return r;
    }
    memcpy((void *)fs_buf, (void *)client_buf, len);
    long r = fs_call(FS_OP_WRITE, f->ino, off, len);
    if (r > 0 && !positional) f->off = off + r;
    return r;
}

static long do_lseek(long fd, int64_t off, int whence)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->type == OF_NULL) return 0;
    if (f->type == OF_SHM) {
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->off : whence == 2 ? (int64_t)mem_shm_size(f->ino) : -1;
        if (base < 0 || base + off < 0) return -EINVAL;
        return f->off = base + off;
    }
    if (f->type == OF_PROCFILE || f->type == OF_PROCDIR) {   /* contenido generado: solo desde el inicio */
        if (whence == 0 && off >= 0) return f->off = off;
        if (whence == 1 && (int64_t)f->off + off >= 0) return f->off += off;
        return -EINVAL;
    }
    if (f->type != OF_FILE && f->type != OF_DIR) return -ESPIPE;
    int64_t base;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = f->off; break;
    case 2: {
        uint64_t size;
        long r = fs_size(f->ino, &size);
        if (r < 0) return r;
        base = size;
        break;
    }
    default: return -EINVAL;
    }
    if (base + off < 0) return -EINVAL;
    f->off = base + off;
    return f->off;
}

static long stat_ino(uint32_t ino)
{
    long r = fs_call(FS_OP_STAT, ino, 0, 0);
    if (r >= 0) memcpy((void *)client_buf, (void *)fs_buf, K_STAT_SIZE);
    return r;
}

static long do_fstat(long fd)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    if (f->type == OF_PROCDIR || f->type == OF_PROCFILE) return procfs_stat(f->path, (void *)client_buf);
    if (f->type == OF_SHM) {
        memset((void *)client_buf, 0, K_STAT_SIZE);
        ((uint32_t *)client_buf)[6] = S_IFREG | 0600;
        ((uint64_t *)client_buf)[2] = 1;
        ((uint64_t *)client_buf)[ST_SIZE_OFFSET / 8] = mem_shm_size(f->ino);
        ((uint64_t *)client_buf)[7] = 4096;
        return 0;
    }
    if (f->type == OF_EVENTFD || f->type == OF_TIMERFD || f->type == OF_EPOLL) {   /* "anon_inode" */
        memset((void *)client_buf, 0, K_STAT_SIZE);
        ((uint32_t *)client_buf)[6] = 0600;
        ((uint64_t *)client_buf)[2] = 1;
        return 0;
    }
    if (f->type == OF_SOCKET || f->type == OF_USOCK) {
        memset((void *)client_buf, 0, K_STAT_SIZE);
        ((uint32_t *)client_buf)[6] = 0140777;                     /* S_IFSOCK */
        return 0;
    }
    if (f->type == OF_TTY || f->type == OF_NULL || f->type == OF_LINUX || f->type == OF_KMSG || f->type == OF_RANDOM) {
        dev_stat(f->type == OF_TTY ? OF_TTY : OF_NULL);
        return 0;
    }
    if (f->type == OF_PIPE_R || f->type == OF_PIPE_W) {
        memset((void *)client_buf, 0, K_STAT_SIZE);
        ((uint32_t *)client_buf)[6] = 0010600;            /* S_IFIFO | 0600 */
        ((uint64_t *)client_buf)[2] = 1;                  /* st_nlink */
        ((uint64_t *)client_buf)[7] = PIPE_SIZE;          /* st_blksize */
        return 0;
    }
    return stat_ino(f->ino);
}

static long do_stat(long dirfd, long flags)
{
    const char *path = client_path(0);
    if (!*path && (flags & AT_EMPTY_PATH)) return do_fstat(dirfd);
    char abs[FS_PATH_MAX];
    long r = normalize(dirfd, path, abs);
    if (r) return r;
    int type = dev_type(abs);
    if (type != OF_FREE) {
        dev_stat(type);
        return 0;
    }
    if (usock_bound(abs, false)) {
        memset((void *)client_buf, 0, K_STAT_SIZE);
        ((uint32_t *)client_buf)[6] = S_IFSOCK_ | 0755;
        ((uint64_t *)client_buf)[2] = 1;
        return 0;
    }
    if (is_proc(abs)) {
        if (!(flags & AT_SYMLINK_NOFOLLOW) && procfs_kind(abs) == PROCFS_LINK) {
            char t[FS_PATH_MAX];
            long n = procfs_readlink(abs, t, sizeof t - 1);
            if (n < 0) return n;
            t[n] = 0;
            if (t[0] == '/') {                        /* exe, cwd: el archivo de verdad */
                long ino = fs_lookup(t, true, NULL, NULL);
                return ino < 0 ? ino : stat_ino(ino);
            }
            memcpy(abs, "/proc/", 6);                 /* self */
            memcpy(abs + 6, t, n + 1);
        }
        return procfs_stat(abs, (void *)client_buf);
    }
    long ino = fs_lookup(abs, !(flags & AT_SYMLINK_NOFOLLOW), NULL, NULL);
    if (ino < 0) return ino;
    return stat_ino(ino);
}

static long do_getdents(long fd, size_t len)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    if (f->type == OF_PROCDIR) return procfs_getdents(f->path, &f->off, (char *)client_buf, len);
    if (f->type != OF_DIR) return -ENOTDIR;
    long r = fs_call(FS_OP_GETDENTS, f->ino, f->off, len);
    if (r >= 0) {
        f->off = microkit_mr_get(1);
        memcpy((void *)client_buf, (void *)fs_buf, r);
    }
    return r;
}

static long do_chdir(const char *abs)
{
    if (is_proc(abs)) {
        if (procfs_kind(abs) != PROCFS_DIR) return -ENOTDIR;
        memcpy(cur->cwd, abs, strlen(abs) + 1);
        return 0;
    }
    uint32_t mode;
    long ino = fs_lookup(abs, true, &mode, NULL);
    if (ino < 0) return ino;
    if (!S_ISDIR(mode)) return -ENOTDIR;
    memcpy(cur->cwd, abs, strlen(abs) + 1);
    return 0;
}

/* Llamadas que reciben una ruta: la normalizan y operan sobre ella */
static long path_op(long nr, long dirfd, long a, long b)
{
    char abs[FS_PATH_MAX];
    long r = normalize(dirfd, client_path(0), abs);
    if (r) return r;
    uint32_t mode;

    switch (nr) {
    case __NR_chdir:
        return do_chdir(abs);
    case __NR_mkdir:
        fs_set_path(0, abs);
        r = fs_call(FS_OP_CREATE, S_IFDIR | (a & 07777 & ~cur->umask), 0, 0);
        return r < 0 ? r : 0;
    case __NR_unlink:
        if (dev_type(abs) != OF_FREE) return -EBUSY;
        if (!a && usock_bound(abs, true)) return 0;     /* nombre de un socket local */
        fs_set_path(0, abs);
        return fs_call(FS_OP_UNLINK, a, 0, 0);       /* a: 1 = rmdir */
    case __NR_readlink: {
        if (is_proc(abs)) return procfs_readlink(abs, (char *)client_buf, a);
        long ino = fs_lookup(abs, false, &mode, NULL);
        if (ino < 0) return ino;
        if (!S_ISLNK(mode)) return -EINVAL;
        r = fs_call(FS_OP_READLINK, ino, 0, 0);
        if (r < 0) return r;
        if ((size_t)r > (size_t)a) r = a;
        memcpy((void *)client_buf, (void *)fs_buf, r);
        return r;
    }
    case __NR_truncate: {
        long ino = fs_lookup(abs, true, &mode, NULL);
        if (ino < 0) return ino;
        if (S_ISDIR(mode)) return -EISDIR;
        return fs_call(FS_OP_TRUNCATE, ino, a, 0);
    }
    case __NR_access: {
        if (dev_type(abs) != OF_FREE) return 0;
        if (is_proc(abs)) return procfs_kind(abs) == PROCFS_NONE ? -ENOENT : 0;
        long ino = fs_lookup(abs, true, &mode, NULL);
        if (ino < 0) return ino;
        /* root: todo permitido salvo ejecutar algo sin ningún bit x */
        if ((a & X_OK) && !S_ISDIR(mode) && !(mode & 0111)) return -EACCES;
        return 0;
    }
    case __NR_chmod: {
        long ino = fs_lookup(abs, true, NULL, NULL);
        if (ino < 0) return ino;
        return fs_call(FS_OP_CHMOD, ino, a, 0);
    }
    case __NR_statfs:
        r = fs_call(FS_OP_STATFS, 0, 0, 0);
        if (r >= 0) memcpy((void *)client_buf, (void *)fs_buf, K_STATFS_SIZE);
        return r;
    }
    return -ENOSYS;
}

/* rename y symlink: dos rutas */
static long do_rename(long olddirfd, long newdirfd)
{
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    long r = normalize(olddirfd, client_path(0), from);
    if (r) return r;
    r = normalize(newdirfd, client_path(POSIX_BUF_PATH2), to);
    if (r) return r;
    fs_set_path(0, from);
    fs_set_path(FS_BUF_PATH2, to);
    return fs_call(FS_OP_RENAME, 0, 0, 0);
}

static long do_symlink(long newdirfd)
{
    char link[FS_PATH_MAX];
    const char *target = client_path(0);          /* el destino se guarda tal cual */
    if (!*target) return -ENOENT;
    long r = normalize(newdirfd, client_path(POSIX_BUF_PATH2), link);
    if (r) return r;
    fs_set_path(0, target);
    fs_set_path(FS_BUF_PATH2, link);
    return fs_call(FS_OP_SYMLINK, 0, 0, 0);
}

/* poll(): struct pollfd[] en el búfer; timeout_ns < 0 = sin plazo */
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020
struct k_pollfd { int fd; short events, revents; };

/* ---------------- sockets locales (AF_UNIX, SOCK_STREAM) ---------------- */

/* Viven en el servidor: cada socket conectado tiene su búfer de entrada (lo que le
 * escribe su par) y una cola de descriptores en tránsito (SCM_RIGHTS: se guardan
 * referencias a la descripción abierta hasta que el otro proceso los recibe).
 * Los nombres (bind) están en una tabla del servidor, no en el disco: unlink y
 * stat de la ruta los ven. Datos de sendmsg/recvmsg: los descriptores van en
 * NET_ADDR_OFF del búfer del proceso. */
#define AF_UNIX_      1
#define NUSOCK        64
#define USOCK_BUF     32768
#define USOCK_MAXFD   32
#define SO_PEERCRED   17
#define S_IFSOCK_     0140000
#define MSG_CMSG_CLOEXEC 0x40000000
#define EADDRINUSE    98
#define ECONNREFUSED  111
#define EPROTONOSUPPORT 93
#define ENOTCONN      107
enum { US_FREE, US_NEW, US_LISTEN, US_CONN };

static struct usock {
    int state;
    int peer;                              /* índice del par (US_CONN) */
    bool peer_closed;
    int pid;                               /* proceso que lo creó (SO_PEERCRED del par) */
    char name[108];                        /* bind: ruta absoluta o "@abstracto" */
    uint32_t head, tail;                   /* búfer de entrada */
    uint8_t buf[USOCK_BUF];
    int fdq[USOCK_MAXFD], nfdq;            /* descripciones abiertas en tránsito (índices de ofiles) */
    int pend[8], npend;                    /* US_LISTEN: conexiones por aceptar */
    uint32_t rwait, wwait;
} usocks[NUSOCK];

/* Nombres de sockets ya cerrados: como el archivo de socket en Linux, siguen ahí
 * hasta el unlink (connect -> ECONNREFUSED, bind -> EADDRINUSE) */
static char stale_names[NUSOCK][108];

static int usock_alloc(void)
{
    for (int i = 0; i < NUSOCK; i++)
        if (usocks[i].state == US_FREE) {
            memset(&usocks[i], 0, offsetof(struct usock, buf));
            usocks[i].state = US_NEW;
            usocks[i].peer = -1;
            usocks[i].pid = cur->pid;
            return i;
        }
    return -1;
}

static void usock_wake(struct usock *u)
{
    proc_wake_mask(u->rwait | u->wwait);
    u->rwait = u->wwait = 0;
}

static void usock_free(int i)
{
    struct usock *u = &usocks[i];
    for (int k = 0; k < u->nfdq; k++) ofile_release(u->fdq[k]);
    for (int k = 0; k < u->npend; k++) usock_free(u->pend[k]);   /* conexiones nunca aceptadas */
    if (u->state == US_CONN && u->peer >= 0) {
        usocks[u->peer].peer_closed = true;
        usock_wake(&usocks[u->peer]);
    }
    if (u->name[0] == '/')                             /* los abstractos desaparecen con el socket */
        for (int k = 0; k < NUSOCK; k++)
            if (!stale_names[k][0]) { memcpy(stale_names[k], u->name, sizeof u->name); break; }
    u->state = US_FREE;
}

/* sockaddr_un en NET_ADDR_OFF del búfer del proceso -> nombre */
static long usock_name(long alen, char *out)
{
    const char *sa = (const char *)(client_buf + NET_ADDR_OFF);
    if (alen <= 2 || alen > 110) return -EINVAL;
    if (sa[2] == 0) {                                   /* espacio abstracto de Linux */
        size_t n = alen - 3 < 106 ? alen - 3 : 106;
        out[0] = '@';
        memcpy(out + 1, sa + 3, n);
        out[n + 1] = 0;
        return 0;
    }
    char path[109], abs[FS_PATH_MAX];
    size_t n = alen - 2;
    memcpy(path, sa + 2, n);
    path[n] = 0;
    long r = normalize(AT_FDCWD, path, abs);
    if (r) return r;
    if (strlen(abs) >= 108) return -ENAMETOOLONG;
    memcpy(out, abs, strlen(abs) + 1);
    return 0;
}

static int usock_lookup(const char *name)
{
    for (int i = 0; i < NUSOCK; i++)
        if (usocks[i].state == US_LISTEN && !strcmp(usocks[i].name, name)) return i;
    return -1;
}

/* Para unlink/stat: ¿hay un socket con este nombre? (y unlink lo quita) */
static bool usock_bound(const char *abs, bool remove)
{
    for (int i = 0; i < NUSOCK; i++)
        if (usocks[i].state != US_FREE && usocks[i].name[0] && !strcmp(usocks[i].name, abs)) {
            if (remove) usocks[i].name[0] = 0;
            return true;
        }
    for (int i = 0; i < NUSOCK; i++)
        if (stale_names[i][0] && !strcmp(stale_names[i], abs)) {
            if (remove) stale_names[i][0] = 0;
            return true;
        }
    return false;
}

static long usock_fd(int ui, long flags)
{
    int of = ofile_alloc(OF_USOCK);
    if (of < 0) return -ENFILE;
    ofiles[of].ino = ui;
    ofiles[of].flags = 2 | (flags & SOCK_NONBLOCK ? O_NONBLOCK : 0);
    int fd = fd_alloc(of, 0, flags & SOCK_CLOEXEC);
    if (fd < 0) ofile_release(of);
    return fd;
}

static long usock_new(long type)
{
    if ((type & 0xf) != 1) return -EPROTONOSUPPORT;     /* solo SOCK_STREAM */
    int u = usock_alloc();
    if (u < 0) return -ENFILE;
    long fd = usock_fd(u, type);
    if (fd < 0) usocks[u].state = US_FREE;
    return fd;
}

static long usock_pair(long type)
{
    if ((type & 0xf) != 1) return -EPROTONOSUPPORT;
    int a = usock_alloc(), b = a >= 0 ? usock_alloc() : -1;
    if (b < 0) {
        if (a >= 0) usocks[a].state = US_FREE;
        return -ENFILE;
    }
    usocks[a].state = usocks[b].state = US_CONN;
    usocks[a].peer = b;
    usocks[b].peer = a;
    long fa = usock_fd(a, type);
    if (fa < 0) { usocks[a].state = usocks[b].state = US_FREE; return fa; }
    long fb = usock_fd(b, type);
    if (fb < 0) { fd_close(fa); return fb; }
    microkit_mr_set(1, fa);
    microkit_mr_set(2, fb);
    return 0;
}

static long usock_bind(struct ofile *f, long alen)
{
    struct usock *u = &usocks[f->ino];
    char name[108];
    long r = usock_name(alen, name);
    if (r) return r;
    if (u->name[0]) return -EINVAL;
    if (usock_bound(name, false)) return -EADDRINUSE;
    if (name[0] == '/') {                               /* como Linux: no si ya existe el archivo */
        uint32_t mode;
        if (fs_lookup(name, false, &mode, NULL) >= 0) return -EADDRINUSE;
    }
    memcpy(u->name, name, sizeof name);
    return 0;
}

static long usock_connect(struct ofile *f, long alen)
{
    struct usock *u = &usocks[f->ino];
    if (u->state == US_CONN) return -EISCONN;
    if (u->state != US_NEW) return -EINVAL;
    char name[108];
    long r = usock_name(alen, name);
    if (r) return r;
    int li = usock_lookup(name);
    if (li < 0) return usock_bound(name, false) || (name[0] == '/' && fs_lookup(name, true, NULL, NULL) >= 0)
                       ? -ECONNREFUSED : -ENOENT;
    struct usock *l = &usocks[li];
    if (l->npend == 8) return -EAGAIN;
    int s = usock_alloc();                              /* el extremo del servidor */
    if (s < 0) return -ENFILE;
    usocks[s].state = u->state = US_CONN;
    usocks[s].peer = f->ino;
    usocks[s].pid = l->pid;
    u->peer = s;
    l->pend[l->npend++] = s;
    usock_wake(l);
    return 0;
}

static long usock_accept(struct ofile *f, long flags)
{
    struct usock *l = &usocks[f->ino];
    if (l->state != US_LISTEN) return -EINVAL;
    if (!l->npend) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        l->rwait |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    int s = l->pend[0];
    memmove(l->pend, l->pend + 1, --l->npend * sizeof(int));
    long fd = usock_fd(s, flags);
    if (fd < 0) usock_free(s);
    microkit_mr_set(1, 0);                              /* el cliente no tiene nombre */
    return fd;
}

/* send: datos al principio de client_buf; nfds descriptores en NET_ADDR_OFF */
static long usock_send(struct ofile *f, size_t len, long flags, long nfds)
{
    struct usock *u = &usocks[f->ino];
    if (u->state != US_CONN) return -ENOTCONN;
    if (u->peer_closed) {
        if (!(flags & MSG_NOSIGNAL)) proc_signal(cur, SIGPIPE);
        return -EPIPE;
    }
    struct usock *p = &usocks[u->peer];
    uint32_t space = USOCK_BUF - (p->tail - p->head);
    if (!space || (nfds && p->nfdq + nfds > USOCK_MAXFD)) {
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
        u->wwait |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    const int *fds = (const int *)(client_buf + NET_ADDR_OFF);
    for (long k = 0; k < nfds; k++) {                   /* comprobar antes de mover nada */
        if (fds[k] < 0 || fds[k] >= NFD || cur->fdt[fds[k]].of < 0) return -EBADF;
    }
    for (long k = 0; k < nfds; k++) {
        int of = cur->fdt[fds[k]].of;
        ofiles[of].refs++;
        p->fdq[p->nfdq++] = of;
    }
    size_t n = len < space ? len : space;
    for (size_t i = 0; i < n; i++) p->buf[(p->tail + i) % USOCK_BUF] = ((const uint8_t *)client_buf)[i];
    p->tail += n;
    usock_wake(p);
    return n;
}

/* recv: datos al principio de client_buf; los descriptores recibidos en NET_ADDR_OFF
 * (MR2 = cuántos; maxfds = cuántos caben del lado del proceso) */
static long usock_recv(struct ofile *f, size_t len, long flags, long maxfds)
{
    struct usock *u = &usocks[f->ino];
    microkit_mr_set(1, 0);
    microkit_mr_set(2, 0);
    if (u->state != US_CONN) return -ENOTCONN;
    uint32_t avail = u->tail - u->head;
    if (!avail) {
        if (u->peer_closed) return 0;
        if ((f->flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) return -EAGAIN;
        u->rwait |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    if (len > POSIX_BUF_SIZE) len = POSIX_BUF_SIZE;
    size_t n = len < avail ? len : avail;
    for (size_t i = 0; i < n; i++) ((uint8_t *)client_buf)[i] = u->buf[(u->head + i) % USOCK_BUF];
    if (!(flags & 2)) u->head += n;                     /* MSG_PEEK */
    /* Los descriptores en tránsito se entregan con los datos (por orden) */
    long got = 0;
    int *out = (int *)(client_buf + NET_ADDR_OFF);
    while (maxfds > 0 && got < maxfds && u->nfdq && !(flags & 2)) {
        int of = u->fdq[0];
        memmove(u->fdq, u->fdq + 1, --u->nfdq * sizeof(int));
        int fd = fd_alloc(of, 0, flags & MSG_CMSG_CLOEXEC);
        if (fd < 0) { ofile_release(of); continue; }
        out[got++] = fd;
    }
    microkit_mr_set(2, got);
    if (u->peer >= 0) usock_wake(&usocks[u->peer]);     /* hay espacio para el escritor */
    return n;
}

static short usock_poll(struct ofile *f, short events, bool wait)
{
    struct usock *u = &usocks[f->ino];
    short r = 0;
    if (u->state == US_LISTEN) {
        if (u->npend) r |= POLLIN;
    } else if (u->state == US_CONN) {
        if (u->tail != u->head || u->peer_closed) r |= POLLIN;
        if (u->peer_closed) r |= POLLHUP;
        else if (usocks[u->peer].tail - usocks[u->peer].head < USOCK_BUF) r |= POLLOUT;
    } else {
        r |= POLLOUT;
    }
    if (!(r & (events | POLLHUP)) && wait) {
        if (events & POLLIN) u->rwait |= 1u << PROC_INDEX(cur);
        if (events & POLLOUT) u->wwait |= 1u << PROC_INDEX(cur);
    }
    return r;
}

/* getsockname/getpeername: sockaddr_un en NET_ADDR_OFF de client_buf, MR1 = longitud */
static long usock_addr(struct ofile *f, bool peer)
{
    struct usock *u = &usocks[f->ino];
    if (peer && u->state != US_CONN) return -ENOTCONN;
    const char *name = peer ? (u->peer >= 0 ? usocks[u->peer].name : "") : u->name;
    char *sa = (char *)(client_buf + NET_ADDR_OFF);
    memset(sa, 0, 110);
    sa[0] = AF_UNIX_;
    size_t n = strlen(name);
    if (name[0] == '@') {                               /* abstracto: \0 + nombre, sin terminador */
        memcpy(sa + 3, name + 1, n - 1);
        n += 2;
    } else {
        memcpy(sa + 2, name, n + 1);
        n = n ? n + 3 : 2;
    }
    microkit_mr_set(1, n);
    return 0;
}

static short fd_poll(int fd, short events, bool wait)
{
    struct ofile *f = fd_get(fd);
    if (!f) return POLLNVAL;
    bool in = events & POLLIN, out = events & POLLOUT;
    short r = 0;
    switch (f->type) {
    case OF_TTY: {
        int t = tty_poll(in, out, wait);
        if (t & 1) r |= POLLIN;
        if (t & 4) r |= POLLOUT;
        break;
    }
    case OF_PIPE_R: {
        struct pipe *pp = &pipes[f->ino];
        if (pp->tail != pp->head) r |= POLLIN;
        if (!pp->writers) r |= POLLHUP;
        if (!r && wait && in) pp->rwait |= 1u << PROC_INDEX(cur);
        break;
    }
    case OF_PIPE_W: {
        struct pipe *pp = &pipes[f->ino];
        if (!pp->readers) r |= POLLERR;
        else if (pp->tail - pp->head < PIPE_SIZE) r |= POLLOUT;
        if (!r && wait && out) pp->wwait |= 1u << PROC_INDEX(cur);
        break;
    }
    case OF_LINUX: {
        int b = bridge_poll(f->ino, in, out, wait);
        if (b & 1) r |= POLLIN;
        if (b & 4) r |= POLLOUT;
        break;
    }
    case OF_EVENTFD: {
        struct evfd *e = &evfds[f->ino];
        if (e->count) r |= POLLIN;
        if (e->count < EVFD_MAX) r |= POLLOUT;
        if (!(r & events) && wait) {
            if (in) e->rwait |= 1u << PROC_INDEX(cur);
            if (out) e->wwait |= 1u << PROC_INDEX(cur);
        }
        break;
    }
    case OF_TIMERFD: {
        struct tfd *t = &tfds[f->ino];
        tfd_update(t, timer_now_ns());
        if (t->expirations) r |= POLLIN;
        else if (wait && in) t->rwait |= 1u << PROC_INDEX(cur);
        break;
    }
    case OF_EPOLL:                         /* anidar epoll en poll: nunca listo (no implementado) */
        break;
    case OF_USOCK:
        r = usock_poll(f, events, wait);
        break;
    case OF_SOCKET: {
        long p = net_call(NET_OP_POLL, f->ino, events, 0, 0);
        r = p < 0 ? POLLERR : p;
        if (!(r & (events | POLLERR | POLLHUP)) && wait) net_waiters |= 1u << PROC_INDEX(cur);
        break;
    }
    default:                               /* archivos, directorios, /dev/null: siempre listos */
        r = POLLIN | POLLOUT;
    }
    return r & (events | POLLERR | POLLHUP | POLLNVAL);
}

static long do_poll(long nfds, long timeout_ns)
{
    if (nfds < 0 || nfds * sizeof(struct k_pollfd) > POSIX_BUF_SIZE) return -EINVAL;
    struct k_pollfd *pf = (struct k_pollfd *)client_buf;
    long ready = 0;
    for (long i = 0; i < nfds; i++) {
        pf[i].revents = pf[i].fd < 0 ? 0 : fd_poll(pf[i].fd, pf[i].events, false);
        if (pf[i].revents) ready++;
    }
    if (ready || timeout_ns == 0) return ready;
    for (long i = 0; i < nfds; i++)                        /* anotarse para que lo despierten */
        if (pf[i].fd >= 0) fd_poll(pf[i].fd, pf[i].events, true);
    if (timeout_ns > 0) timer_wait(cur, timeout_ns);
    return POSIX_RETRY;
}

/* utimensat(): buf = ruta (vacía = el propio dirfd); buf+PATH2 = timespec[2] si a3 != 0 */
#define UTIME_NOW  ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
static long do_utimensat(long dirfd, long have_times, long flags)
{
    uint32_t ino;
    const char *path = client_path(0);
    if (!*path) {
        struct ofile *f = fd_get(dirfd);
        if (!f) return -EBADF;
        if (f->type != OF_FILE && f->type != OF_DIR) return -EINVAL;
        ino = f->ino;
    } else {
        char abs[FS_PATH_MAX];
        long r = normalize(dirfd, path, abs);
        if (r) return r;
        if (dev_type(abs) != OF_FREE) return 0;
        long i = fs_lookup(abs, !(flags & AT_SYMLINK_NOFOLLOW), NULL, NULL);
        if (i < 0) return i;
        ino = i;
    }
    int64_t t[2] = { (int64_t)clock_now(), (int64_t)clock_now() };
    if (have_times) {
        const int64_t *ts = (const int64_t *)(client_buf + POSIX_BUF_PATH2);   /* {sec, nsec} x 2 */
        for (int k = 0; k < 2; k++) {
            if (ts[2 * k + 1] == UTIME_OMIT) t[k] = -1;
            else if (ts[2 * k + 1] != UTIME_NOW) t[k] = ts[2 * k];
        }
    }
    return fs_call(FS_OP_SETTIMES, ino, t[0], t[1]);
}

/* chown/lchown/fchownat: buf = ruta (vacía + AT_EMPTY_PATH = el propio dirfd) */
static long do_chown(long dirfd, long uid, long gid, long flags)
{
    const char *path = client_path(0);
    if (!*path && (flags & AT_EMPTY_PATH)) {
        struct ofile *f = fd_get(dirfd);
        if (!f) return -EBADF;
        return f->type == OF_FILE || f->type == OF_DIR ? fs_call(FS_OP_CHOWN, f->ino, uid, gid) : 0;
    }
    char abs[FS_PATH_MAX];
    long r = normalize(dirfd, path, abs);
    if (r) return r;
    if (dev_type(abs) != OF_FREE) return 0;
    long ino = fs_lookup(abs, !(flags & AT_SYMLINK_NOFOLLOW), NULL, NULL);
    if (ino < 0) return ino;
    return fs_call(FS_OP_CHOWN, ino, uid, gid);
}

static long do_dup(long oldfd, long newfd, bool exact, bool cloexec)
{
    struct ofile *f = fd_get(oldfd);
    if (!f) return -EBADF;
    if (exact) {
        if (newfd < 0 || newfd >= NFD) return -EBADF;
        if (newfd == oldfd) return newfd;
        if (cur->fdt[newfd].of >= 0) fd_close(newfd);
    }
    int fd = fd_alloc(cur->fdt[oldfd].of, exact ? newfd : 0, cloexec);
    if (fd >= 0) f->refs++;
    return fd;
}

static long do_fcntl(long fd, long cmd, long arg)
{
    struct ofile *f = fd_get(fd);
    if (!f) return -EBADF;
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int nfd = fd_alloc(cur->fdt[fd].of, arg, cmd == F_DUPFD_CLOEXEC);
        if (nfd >= 0) f->refs++;
        return nfd;
    }
    case F_GETFD: return cur->fdt[fd].cloexec ? FD_CLOEXEC : 0;
    case F_SETFD: cur->fdt[fd].cloexec = arg & FD_CLOEXEC; return 0;
    case F_GETFL: return f->flags;
    case F_SETFL: f->flags = (f->flags & ~(O_APPEND | O_NONBLOCK)) | (arg & (O_APPEND | O_NONBLOCK)); return 0;
    case F_GETLK: case F_SETLK: case F_SETLKW: return 0;     /* un solo proceso: sin bloqueos */
    case 1033: case 1034: return f->type == OF_SHM ? 0 : -EINVAL;   /* F_ADD_SEALS, F_GET_SEALS: sin sellos */
    }
    return -EINVAL;
}

bool vfs_handles(long nr)
{
    switch (nr) {
    case __NR_read: case __NR_write: case __NR_pread64: case __NR_pwrite64:
    case __NR_open: case __NR_openat: case __NR_close: case __NR_lseek:
    case __NR_stat: case __NR_lstat: case __NR_fstat: case __NR_newfstatat:
    case __NR_getdents64: case __NR_getcwd: case __NR_chdir: case __NR_fchdir:
    case __NR_mkdir: case __NR_mkdirat: case __NR_rmdir: case __NR_unlink: case __NR_unlinkat:
    case __NR_rename: case __NR_renameat: case __NR_renameat2:
    case __NR_symlink: case __NR_symlinkat: case __NR_readlink: case __NR_readlinkat:
    case __NR_truncate: case __NR_ftruncate: case __NR_access: case __NR_faccessat: case __NR_faccessat2:
    case __NR_chmod: case __NR_fchmod: case __NR_fchmodat: case __NR_umask:
    case __NR_statfs: case __NR_fstatfs: case __NR_fcntl: case __NR_ioctl:
    case __NR_dup: case __NR_dup2: case __NR_dup3:
    case __NR_fsync: case __NR_fdatasync: case __NR_sync:
    case __NR_pipe2: case __NR_poll: case __NR_utimensat:
    case __NR_chown: case __NR_lchown: case __NR_fchown: case __NR_fchownat:
    case __NR_socket: case __NR_bind: case __NR_connect: case __NR_listen:
    case __NR_accept: case __NR_accept4: case __NR_sendto: case __NR_recvfrom:
    case __NR_shutdown: case __NR_setsockopt: case __NR_getsockopt:
    case __NR_getsockname: case __NR_getpeername: case __NR_socketpair:
    case __NR_eventfd: case __NR_eventfd2: case __NR_timerfd_create:
    case __NR_timerfd_settime: case __NR_timerfd_gettime:
    case __NR_epoll_create: case __NR_epoll_create1:
    case __NR_memfd_create: case __NR_mmap: case __NR_flock:
        return true;
    }
    return false;
}

long vfs_syscall(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    (void)a5;
    struct ofile *f;
    switch (nr) {
    case __NR_read:       return do_read(a1, a3, false, 0);
    case __NR_write:      return do_write(a1, a3, false, 0);
    case __NR_pread64:    return do_read(a1, a3, true, a4);
    case __NR_pwrite64:   return do_write(a1, a3, true, a4);
    case __NR_open:       return do_open(AT_FDCWD, a2, a3);
    case __NR_openat:     return do_open(a1, a3, a4);
    case __NR_close:      return fd_close(a1);
    case __NR_lseek:      return do_lseek(a1, a2, a3);
    case __NR_stat:       return do_stat(AT_FDCWD, 0);
    case __NR_lstat:      return do_stat(AT_FDCWD, AT_SYMLINK_NOFOLLOW);
    case __NR_fstat:      return do_fstat(a1);
    case __NR_newfstatat: return do_stat(a1, a4);
    case __NR_getdents64: return do_getdents(a1, a3);
    case __NR_getcwd: {
        size_t len = strlen(cur->cwd) + 1;
        if ((size_t)a2 < len) return -ERANGE;
        memcpy((void *)client_buf, cur->cwd, len);
        return len;
    }
    case __NR_chdir:      return path_op(__NR_chdir, AT_FDCWD, 0, 0);
    case __NR_fchdir:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type != OF_DIR && f->type != OF_PROCDIR) return -ENOTDIR;
        memcpy(cur->cwd, f->path, strlen(f->path) + 1);
        return 0;
    case __NR_mkdir:      return path_op(__NR_mkdir, AT_FDCWD, a2, 0);
    case __NR_mkdirat:    return path_op(__NR_mkdir, a1, a3, 0);
    case __NR_rmdir:      return path_op(__NR_unlink, AT_FDCWD, 1, 0);
    case __NR_unlink:     return path_op(__NR_unlink, AT_FDCWD, 0, 0);
    case __NR_unlinkat:   return path_op(__NR_unlink, a1, (a3 & AT_REMOVEDIR) ? 1 : 0, 0);
    case __NR_rename:     return do_rename(AT_FDCWD, AT_FDCWD);
    case __NR_renameat:   return do_rename(a1, a3);
    case __NR_renameat2:  return a5 ? -EINVAL : do_rename(a1, a3);
    case __NR_symlink:    return do_symlink(AT_FDCWD);
    case __NR_symlinkat:  return do_symlink(a2);
    case __NR_readlink:   return path_op(__NR_readlink, AT_FDCWD, a3, 0);
    case __NR_readlinkat: return path_op(__NR_readlink, a1, a4, 0);
    case __NR_truncate:   return path_op(__NR_truncate, AT_FDCWD, a2, 0);
    case __NR_ftruncate:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type == OF_SHM) return a2 < 0 ? -EINVAL : mem_shm_truncate(f->ino, a2);
        if (f->type != OF_FILE || (f->flags & O_ACCMODE) == O_RDONLY) return -EINVAL;
        return fs_call(FS_OP_TRUNCATE, f->ino, a2, 0);
    case __NR_access:     return path_op(__NR_access, AT_FDCWD, a2, 0);
    case __NR_faccessat:
    case __NR_faccessat2: return path_op(__NR_access, a1, a3, 0);
    case __NR_chmod:      return path_op(__NR_chmod, AT_FDCWD, a2, 0);
    case __NR_fchmodat:   return path_op(__NR_chmod, a1, a3, 0);
    case __NR_fchmod:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type != OF_FILE && f->type != OF_DIR) return -EINVAL;
        return fs_call(FS_OP_CHMOD, f->ino, a2, 0);
    case __NR_umask: {
        long old = cur->umask;
        cur->umask = a1 & 0777;
        return old;
    }
    case __NR_statfs:     return path_op(__NR_statfs, AT_FDCWD, 0, 0);
    case __NR_fstatfs: {
        if (!fd_get(a1)) return -EBADF;
        long r = fs_call(FS_OP_STATFS, 0, 0, 0);
        if (r >= 0) memcpy((void *)client_buf, (void *)fs_buf, K_STATFS_SIZE);
        return r;
    }
    case __NR_fcntl:      return do_fcntl(a1, a2, a3);
    case __NR_ioctl:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (a2 == FIONBIO) {                               /* arg: el entero, por valor */
            f->flags = a3 ? f->flags | O_NONBLOCK : f->flags & ~O_NONBLOCK;
            return 0;
        }
        if (f->type == OF_SOCKET && a2 == NET_SIOC_CONFIG) {    /* configurar la red (apps/red) */
            memcpy((void *)net_buf, (void *)client_buf, sizeof(struct net_cfg));
            long r = net_call(NET_OP_CONFIG, 0, 0, 0, 0);
            if (r == 0) net_lease_changed();                    /* DNS de la dirección fija */
            return r;
        }
        if (f->type == OF_SOCKET && a2 == FIONREAD) {
            long n = net_call(NET_OP_FIONREAD, f->ino, 0, 0, 0);
            if (n >= 0) microkit_mr_set(1, n);
            return n < 0 ? n : 0;
        }
        return f->type == OF_TTY ? tty_ioctl(a2, a3) : -ENOTTY;
    case __NR_dup:        return do_dup(a1, 0, false, false);
    case __NR_dup2:       return do_dup(a1, a2, true, false);
    case __NR_dup3:       return a1 == a2 ? -EINVAL : do_dup(a1, a2, true, a3 & O_CLOEXEC);
    case __NR_fsync:
    case __NR_fdatasync:  return fd_get(a1) ? 0 : -EBADF;   /* fs_ext2 escribe inmediatamente */
    case __NR_sync:       return 0;
    case __NR_pipe2:      return do_pipe(a1);
    case __NR_poll:       return do_poll(a1, a2);
    case __NR_utimensat:  return do_utimensat(a1, a3, a4);
    case __NR_chown:      return do_chown(AT_FDCWD, a2, a3, 0);
    case __NR_lchown:     return do_chown(AT_FDCWD, a2, a3, AT_SYMLINK_NOFOLLOW);
    case __NR_fchownat:   return do_chown(a1, a3, a4, a5);
    case __NR_fchown:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type != OF_FILE && f->type != OF_DIR) return 0;   /* tty, tuberías: sin dueño en disco */
        return fs_call(FS_OP_CHOWN, f->ino, a2, a3);
    }
    switch (nr) {
    case __NR_memfd_create: {                          /* a2: MFD_CLOEXEC (1), MFD_ALLOW_SEALING */
        long id = mem_shm_new();
        if (id < 0) return id;
        int of = ofile_alloc(OF_SHM);
        if (of < 0) { mem_shm_unref(id); return -ENFILE; }
        ofiles[of].ino = id;
        ofiles[of].flags = 2;                          /* O_RDWR */
        int fd = fd_alloc(of, 0, a2 & 1);
        if (fd < 0) ofile_release(of);
        return fd;
    }
    case __NR_mmap:                                    /* solo memfd compartido; lo demás lo hace el runtime */
        f = fd_get(a5);
        if (!f) return -EBADF;
        if (f->type != OF_SHM || !(a4 & 1)) return -ENODEV;   /* MAP_SHARED */
        return mem_shm_map(cur->slot, f->ino, a1, a2, a6, a3 & 2);   /* PROT_WRITE */
    case __NR_flock:                                   /* bloqueos de archivo: un solo usuario, se aceptan */
        return fd_get(a1) ? 0 : -EBADF;
    case __NR_eventfd:         return evfd_new(a1, 0);
    case __NR_eventfd2:        return evfd_new(a1, a2);
    case __NR_timerfd_create:  return tfd_new(a2);
    case __NR_epoll_create:
    case __NR_epoll_create1: {
        int of = ofile_alloc(OF_EPOLL);
        if (of < 0) return -ENFILE;
        int fd = fd_alloc(of, 0, nr == __NR_epoll_create1 && (a1 & O_CLOEXEC));
        if (fd < 0) ofile_release(of);
        return fd;
    }
    case __NR_timerfd_settime:
    case __NR_timerfd_gettime:
        f = fd_get(a1);
        if (!f) return -EBADF;
        if (f->type != OF_TIMERFD) return -EINVAL;
        return tfd_settime(f, nr == __NR_timerfd_settime, a2, a3);
    }
    if (nr == __NR_socket) return sock_socket(a1, a2, a3);
    if (nr == __NR_socketpair) return a1 == AF_UNIX_ ? usock_pair(a2) : -EOPNOTSUPP;
    f = fd_get(a1);
    if (!f) return -EBADF;
    if (f->type == OF_USOCK) {
        switch (nr) {
        case __NR_bind:       return usock_bind(f, a3);
        case __NR_listen:     if (usocks[f->ino].state == US_NEW) usocks[f->ino].state = US_LISTEN; return 0;
        case __NR_connect:    return usock_connect(f, a3);
        case __NR_accept:     return usock_accept(f, 0);
        case __NR_accept4:    return usock_accept(f, a4);
        case __NR_sendto:     return usock_send(f, a3, a4, a6);
        case __NR_recvfrom:   return usock_recv(f, a3, a4, a5);
        case __NR_shutdown: {
            struct usock *u = &usocks[f->ino];
            if ((a2 == 1 || a2 == 2) && u->state == US_CONN) {        /* SHUT_WR/RDWR: EOF al par */
                usocks[u->peer].peer_closed = true;
                usock_wake(&usocks[u->peer]);
            }
            return 0;
        }
        case __NR_setsockopt: return 0;
        case __NR_getsockopt: {
            struct usock *u = &usocks[f->ino];
            if (a2 == SOL_SOCKET && a3 == SO_PEERCRED) {                 /* struct ucred */
                int cred[3] = { u->peer >= 0 ? usocks[u->peer].pid : 0, 0, 0 };
                memcpy((void *)client_buf, cred, 12);
                microkit_mr_set(1, 12);
                return 0;
            }
            int v = a3 == 3 /* SO_TYPE */ ? 1 : 0;
            memcpy((void *)client_buf, &v, 4);
            microkit_mr_set(1, 4);
            return 0;
        }
        case __NR_getsockname: return usock_addr(f, false);
        case __NR_getpeername: return usock_addr(f, true);
        }
        return -EOPNOTSUPP;
    }
    if (f->type != OF_SOCKET) return -ENOTSOCK;
    switch (nr) {
    case __NR_bind:        return sock_addr_call(NET_OP_BIND, f, a3);
    case __NR_connect:     return sock_connect(f, a3);
    case __NR_listen:      return net_call(NET_OP_LISTEN, f->ino, a2, 0, 0);
    case __NR_accept:      return sock_accept(f, 0);
    case __NR_accept4:     return sock_accept(f, a4);
    case __NR_sendto:      return sock_send(f, a3, a4, a5 > NET_ADDR_MAX ? NET_ADDR_MAX : a5);
    case __NR_recvfrom:    return sock_recv(f, a3, a4);
    case __NR_shutdown:    return net_call(NET_OP_SHUTDOWN, f->ino, a2, 0, 0);
    case __NR_setsockopt:
        if (a5 > NET_DATA_MAX) return -EINVAL;
        memcpy((void *)net_buf, (void *)client_buf, a5);
        return net_call(NET_OP_SETSOCKOPT, f->ino, a2, a3, a5);
    case __NR_getsockopt: {
        long r = net_call(NET_OP_GETSOCKOPT, f->ino, a2, a3, a4);
        if (r >= 0) {
            long olen = microkit_mr_get(1);
            memcpy((void *)client_buf, (void *)net_buf, olen);
            microkit_mr_set(1, olen);
        }
        return r;
    }
    case __NR_getsockname:
    case __NR_getpeername: {
        long r = net_call(nr == __NR_getsockname ? NET_OP_GETSOCKNAME : NET_OP_GETPEERNAME, f->ino, 0, 0, 0);
        if (r >= 0) sock_addr_out();
        return r;
    }
    }
    return -ENOSYS;
}

void vfs_init(void)
{
}

/* Pasa el reloj a fs_ext2 para fechar los archivos */
void vfs_set_fs_clock(void)
{
    fs_call(FS_OP_SETCLOCK, clock_epoch, clock_tsc0, clock_tsc_hz);
}

/* 0, 1 y 2 a la terminal (proceso inicial) */
void vfs_proc_tty(struct proc *p)
{
    for (int i = 0; i < NFD; i++) p->fdt[i].of = -1;
    int tty = ofile_alloc(OF_TTY);
    ofiles[tty].flags = 2;  /* O_RDWR */
    memcpy(ofiles[tty].path, "/dev/tty", 9);
    ofiles[tty].refs = 3;
    for (int fd = 0; fd < 3; fd++) p->fdt[fd].of = tty;
    memcpy(p->cwd, "/", 2);
    p->umask = 022;
}

/* fork: el hijo hereda los descriptores (comparten la descripción abierta) */
void vfs_fork(struct proc *child, struct proc *parent)
{
    for (int i = 0; i < NFD; i++) {
        child->fdt[i] = parent->fdt[i];
        if (child->fdt[i].of >= 0) ofiles[child->fdt[i].of].refs++;
    }
    memcpy(child->cwd, parent->cwd, strlen(parent->cwd) + 1);
    child->umask = parent->umask;
}

void vfs_close_all(struct proc *p)
{
    for (int i = 0; i < NFD; i++) {
        if (p->fdt[i].of >= 0) {
            ofile_release(p->fdt[i].of);
            p->fdt[i].of = -1;
        }
    }
}

void vfs_close_cloexec(struct proc *p)
{
    for (int i = 0; i < NFD; i++) {
        if (p->fdt[i].of >= 0 && p->fdt[i].cloexec) {
            ofile_release(p->fdt[i].of);
            p->fdt[i].of = -1;
        }
    }
}

long vfs_normalize(struct proc *p, const char *path, char *out)
{
    struct proc *saved = cur;
    cur = p;
    long r = normalize(AT_FDCWD, path, out);
    cur = saved;
    return r;
}

long vfs_lookup(const char *abs, bool follow, uint32_t *mode, uint64_t *size)
{
    return fs_lookup(abs, follow, mode, size);
}

/* Lee de un archivo a memoria del servidor (para el cargador de programas) */
long vfs_read_file(uint32_t ino, uint64_t off, void *dst, size_t len)
{
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done < FS_BUF_SIZE ? len - done : FS_BUF_SIZE;
        long r = fs_call(FS_OP_READ, ino, off + done, chunk);
        if (r < 0) return r;
        if (r == 0) break;
        memcpy((char *)dst + done, (void *)fs_buf, r);
        done += r;
    }
    return done;
}
