/*
 * posix_server VFS: pipes and named pipes (FIFOs) (see vfs.h).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vfs.h"

struct pipe pipes[NPIPE];

void pipe_maybe_free(struct pipe *pp)
{
    if (!pp->readers && !pp->writers) pp->used = false;
}

/* A process blocked opening a FIFO already counts as a reader or writer (as in Linux); if it
 * gives up (signal, exit, another open) that count goes away */
void fifo_cancel_wait(struct proc *p)
{
    if (!p->fifo_wait) return;
    struct pipe *pp = &pipes[p->fifo_wait - 1];
    if (p->fifo_wait_write) pp->writers--; else pp->readers--;
    p->fifo_wait = 0;
    proc_wake_mask(pp->rwait | pp->wwait | pp->owait);
    pp->rwait = pp->wwait = pp->owait = 0;
    pipe_maybe_free(pp);
}

long pipe_read(struct ofile *f, size_t len)
{
    struct pipe *pp = &pipes[f->ino];
    uint32_t avail = pp->tail - pp->head;
    if (!avail) {
        if (!pp->writers) return 0;                       /* end of file */
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        pp->rwait |= WAIT_BIT(cur);
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

long pipe_write(struct ofile *f, size_t len)
{
    struct pipe *pp = &pipes[f->ino];
    if (!pp->readers) {
        proc_signal(cur, SIGPIPE);
        return -EPIPE;
    }
    uint32_t space = PIPE_SIZE - (pp->tail - pp->head);
    if (!space) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        pp->wwait |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    size_t n = len < space ? len : space;
    for (size_t i = 0; i < n; i++) pp->buf[(pp->tail + i) % PIPE_SIZE] = ((const char *)client_buf)[i];
    pp->tail += n;
    proc_wake_mask(pp->rwait);
    pp->rwait = 0;
    return n;
}

long do_pipe(long flags)
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
    pp->rwait = pp->wwait = pp->owait = 0;
    pp->fifo = 0;
    ofiles[r].ino = ofiles[w].ino = pi;
    ofiles[r].flags = flags & O_NONBLOCK;
    ofiles[w].flags = 1 | (flags & O_NONBLOCK);       /* O_WRONLY */
    microkit_mr_set(1, fr);
    microkit_mr_set(2, fw);
    return 0;
}

/* open() of a named pipe, with Linux's semantics: for reading or writing alone, open waits for
 * the other end; with O_NONBLOCK reading opens at once and writing fails with ENXIO; O_RDWR
 * never waits. As in Linux, an opener counts as a reader or writer from the moment it enters,
 * even while it waits, so two blocked opens on both ends meet each other. */
long fifo_open(uint32_t ino, uint32_t flags, const char *abs)
{
    int pi = -1, free_pi = -1;
    for (int i = 0; i < NPIPE; i++) {
        if (pipes[i].used && pipes[i].fifo == ino) { pi = i; break; }
        if (!pipes[i].used && free_pi < 0) free_pi = i;
    }
    if (cur->fifo_wait && cur->fifo_wait != pi + 1) fifo_cancel_wait(cur);   /* gave up on another one */
    if (pi < 0) {
        if (free_pi < 0) return -ENFILE;
        pi = free_pi;
        struct pipe *np = &pipes[pi];
        np->used = true;
        np->fifo = ino;
        np->readers = np->writers = 0;
        np->head = np->tail = 0;
        np->rwait = np->wwait = np->owait = 0;
        np->r_opens = np->w_opens = 0;
    }
    struct pipe *pp = &pipes[pi];
    uint32_t acc = flags & O_ACCMODE;
    bool rd = acc == O_RDONLY;
    if (cur->fifo_wait != pi + 1) {                     /* first time here (not a retry) */
        if (acc == O_WRONLY && (flags & O_NONBLOCK) && !pp->readers) {
            pipe_maybe_free(pp);
            return -ENXIO;                              /* write end, nobody reading */
        }
        if (acc != O_WRONLY) { pp->readers++; pp->r_opens++; }
        if (acc != O_RDONLY) { pp->writers++; pp->w_opens++; }
        proc_wake_mask(pp->owait | pp->rwait | pp->wwait);   /* the other end may be waiting for us */
        pp->owait = pp->rwait = pp->wwait = 0;
    }
    /* wait until the other end is open, or was opened since we started (it may be gone already) */
    uint32_t other = rd ? pp->w_opens : pp->r_opens;
    bool retry = cur->fifo_wait == pi + 1;
    if (acc != O_RDWR && !(flags & O_NONBLOCK) && !(rd ? pp->writers : pp->readers) &&
        !(retry && other != cur->fifo_wait_opens)) {
        if (!retry) cur->fifo_wait_opens = other;
        cur->fifo_wait = pi + 1;                        /* keeps its count while it waits */
        cur->fifo_wait_write = !rd;
        pp->owait |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    cur->fifo_wait = 0;
    int of = ofile_alloc(acc == O_WRONLY ? OF_PIPE_W : OF_PIPE_R);
    if (of < 0) {
        if (acc != O_WRONLY) pp->readers--;
        if (acc != O_RDONLY) pp->writers--;
        pipe_maybe_free(pp);
        return -ENFILE;
    }
    ofiles[of].ino = pi;
    ofiles[of].flags = flags & (O_ACCMODE | O_NONBLOCK | O_APPEND);
    memcpy(ofiles[of].path, abs, strlen(abs) + 1);
    int fd = fd_alloc(of, 0, flags & O_CLOEXEC);
    if (fd < 0) ofile_release(of);
    return fd;
}
