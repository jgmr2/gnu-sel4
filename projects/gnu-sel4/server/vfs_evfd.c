/*
 * posix_server VFS: eventfd and timerfd (see vfs.h).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "vfs.h"

struct evfd evfds[NEVFD];
struct tfd tfds[NTFD];

/* ---------------- eventfd and timerfd ---------------- */

#define EFD_SEMAPHORE 1
#define EVFD_MAX 0xfffffffffffffffeULL

long evfd_new(uint64_t init, long flags)
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

long evfd_read(struct ofile *f, size_t len)
{
    struct evfd *e = &evfds[f->ino];
    if (len < 8) return -EINVAL;
    if (!e->count) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        e->rwait |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    uint64_t v = e->semaphore ? 1 : e->count;
    e->count -= v;
    memcpy((void *)client_buf, &v, 8);
    proc_wake_mask(e->wwait);
    e->wwait = 0;
    return 8;
}

long evfd_write(struct ofile *f, size_t len)
{
    struct evfd *e = &evfds[f->ino];
    uint64_t v;
    if (len < 8) return -EINVAL;
    memcpy(&v, (void *)client_buf, 8);
    if (v == ~0ULL) return -EINVAL;
    if (v > EVFD_MAX - e->count) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        e->wwait |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    e->count += v;
    proc_wake_mask(e->rwait);
    e->rwait = 0;
    return 8;
}

long tfd_new(long flags)
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

/* Accumulates the expirations that are due */
void tfd_update(struct tfd *t, uint64_t now)
{
    if (!t->next || now < t->next) return;
    uint64_t n = 1 + (t->interval ? (now - t->next) / t->interval : 0);
    t->expirations += n;
    t->next = t->interval ? t->next + n * t->interval : 0;
}

/* settime: value (relative, ns; 0 = disarm) and interval; the previous ones in MR1/MR2 */
long tfd_settime(struct ofile *f, bool set, uint64_t value, uint64_t interval)
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

long tfd_read(struct ofile *f, size_t len)
{
    struct tfd *t = &tfds[f->ino];
    if (len < 8) return -EINVAL;
    tfd_update(t, timer_now_ns());
    if (!t->expirations) {
        if (f->flags & O_NONBLOCK) return -EAGAIN;
        t->rwait |= WAIT_BIT(cur);
        return POSIX_RETRY;
    }
    memcpy((void *)client_buf, &t->expirations, 8);
    t->expirations = 0;
    return 8;
}

/* For clock.c: the nearest timerfd deadline, and waking whoever waits on expired ones */
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

short evfd_poll(struct ofile *f, short events, bool in, bool out, bool wait)
{
    struct evfd *e = &evfds[f->ino];
    short r = 0;
    if (e->count) r |= POLLIN;
    if (e->count < EVFD_MAX) r |= POLLOUT;
    if (!(r & events) && wait) {
        if (in) e->rwait |= WAIT_BIT(cur);
        if (out) e->wwait |= WAIT_BIT(cur);
    }
    return r;
}

short tfd_poll(struct ofile *f, bool in, bool wait)
{
    struct tfd *t = &tfds[f->ino];
    tfd_update(t, timer_now_ns());
    if (t->expirations) return POLLIN;
    if (wait && in) t->rwait |= WAIT_BIT(cur);
    return 0;
}
