/*
 * Bridge to the Linux VM (see include/sel4posix/bridge.h).
 *
 * The channel is two byte queues in sDDF serial format, in memory shared with
 * CLIENT_VMM, which presents them to Linux as the virtio console /dev/hvc1:
 *   br_rx: posix_server produces, the VM consumes (towards the agent)
 *   br_tx: the VM produces, posix_server consumes (from the agent)
 *
 * Each open of /dev/linux is a session. Frames arriving from the agent are
 * kept in the session's inbox until the program reads them.
 */
#include <os/sddf.h>
#include <string.h>
#include <sddf/serial/queue.h>
#include <sel4posix/bridge.h>
#include "server.h"

/* Regions shared with CLIENT_VMM (set by Microkit, see meta.py) */
uintptr_t br_rx_queue, br_rx_data, br_tx_queue, br_tx_data;
#define BR_DATA_SIZE 0x10000

static serial_queue_handle_t rxq, txq;
static bool agent_ready;
static uint32_t rx_waiters, hello_waiters;      /* processes waiting for room / for the agent */

#define NSESSION   16
#define INBOX_SIZE 0x10000
static struct session {
    bool used, dead;
    bool started;                               /* it already sent something to the agent */
    uint32_t head, tail;                        /* circular inbox of records */
    uint32_t waiters;
    uint8_t inbox[INBOX_SIZE];
} sessions[NSESSION];

/* ---------------- sending to the VM ---------------- */

static uint32_t rx_free(void) { return serial_queue_free(&rxq); }

static void rx_put(const void *data, uint32_t len)
{
    const char *p = data;
    for (uint32_t i = 0; i < len; i++) serial_enqueue(&rxq, p[i]);
}

/* Queues a whole frame or nothing at all (POSIX_RETRY if it does not fit) */
static long send_frame(uint8_t type, int session, const void *data, uint32_t len)
{
    if (rx_free() < sizeof(struct br_frame) + len) {
        serial_request_consumer_signal(&rxq);
        if (rx_free() < sizeof(struct br_frame) + len) {
            rx_waiters |= 1u << PROC_INDEX(cur);
            return POSIX_RETRY;
        }
    }
    struct br_frame f = { .type = type, .session = session, .len = len };
    rx_put(&f, sizeof f);
    rx_put(data, len);
    microkit_notify(BRIDGE_CH);
    return len;
}

/* ---------------- receiving from the VM ---------------- */

static uint32_t inbox_free(struct session *s) { return INBOX_SIZE - (s->tail - s->head); }

static void inbox_put(struct session *s, const void *data, uint32_t len)
{
    const uint8_t *p = data;
    for (uint32_t i = 0; i < len; i++) s->inbox[(s->tail + i) % INBOX_SIZE] = p[i];
    s->tail += len;
}

static void inbox_get(struct session *s, void *data, uint32_t len)
{
    uint8_t *p = data;
    for (uint32_t i = 0; i < len; i++) p[i] = s->inbox[(s->head + i) % INBOX_SIZE];
    s->head += len;
}

/* The agent (re)started: sessions that were already talking to the previous one
 * are dead; those waiting for it to arrive stay alive */
static void kill_started_sessions(void)
{
    for (int i = 0; i < NSESSION; i++) {
        if (sessions[i].used && sessions[i].started && !sessions[i].dead) {
            sessions[i].dead = true;
            proc_wake_mask(sessions[i].waiters);
            sessions[i].waiters = 0;
        }
    }
}

/* Peeks (without consuming) at n bytes of the queue from the VM */
static bool tx_peek(void *dst, uint32_t off, uint32_t n)
{
    if (serial_queue_length_consumer(&txq) < off + n) return false;
    uint8_t *p = dst;
    for (uint32_t i = 0; i < n; i++)
        p[i] = txq.data_region[(txq.queue->head + off + i) % txq.capacity];
    return true;
}

static void tx_skip(uint32_t n)
{
    serial_update_shared_head(&txq, txq.queue->head + n);
}

/* Processes every complete frame in the queue from the VM. A frame for a
 * full inbox is left in the queue (back-pressure towards the VM). */
static void pump(void)
{
    bool consumed = false;
    for (;;) {
        struct br_frame f;
        if (!tx_peek(&f, 0, sizeof f)) break;
        if (f.len > BR_MAX_DATA) {                         /* corrupt frame: drop everything */
            tx_skip(serial_queue_length_consumer(&txq));
            consumed = true;
            break;
        }
        if (serial_queue_length_consumer(&txq) < sizeof f + f.len) break;
        if (f.type == BR_HELLO) {
            kill_started_sessions();
            agent_ready = true;
            proc_wake_mask(hello_waiters);
            hello_waiters = 0;
            tx_skip(sizeof f + f.len);
            consumed = true;
            continue;
        }
        struct session *s = f.session < NSESSION ? &sessions[f.session] : NULL;
        if (!s || !s->used || s->dead) {                   /* closed session: drop it */
            tx_skip(sizeof f + f.len);
            consumed = true;
            continue;
        }
        uint32_t rec = 1 + 4 + f.len;                      /* record: type, length, data */
        if (inbox_free(s) < rec) break;
        uint8_t hdr[5] = { f.type, f.len, f.len >> 8, f.len >> 16, f.len >> 24 };
        inbox_put(s, hdr, 5);
        for (uint32_t i = 0; i < f.len; i++) {
            uint8_t c = txq.data_region[(txq.queue->head + sizeof f + i) % txq.capacity];
            inbox_put(s, &c, 1);
        }
        tx_skip(sizeof f + f.len);
        consumed = true;
        if (f.type == BR_EXIT) s->dead = true;
        proc_wake_mask(s->waiters);
        s->waiters = 0;
    }
    if (consumed && serial_require_consumer_signal(&txq)) {
        serial_cancel_consumer_signal(&txq);
        microkit_notify(BRIDGE_CH);
    }
}

void bridge_notified(void)
{
    pump();
    if (rx_waiters) {
        proc_wake_mask(rx_waiters);
        rx_waiters = 0;
    }
}

/* ---------------- interface for the VFS ---------------- */

long bridge_open(void)
{
    for (int i = 0; i < NSESSION; i++) {
        if (!sessions[i].used) {
            struct session *s = &sessions[i];
            s->used = true;
            s->dead = false;
            s->started = false;
            s->head = s->tail = 0;
            s->waiters = 0;
            return i;
        }
    }
    return -EAGAIN;
}

void bridge_close(int sid)
{
    struct session *s = &sessions[sid];
    if (!s->used) return;
    if (!s->dead && agent_ready && rx_free() >= sizeof(struct br_frame)) {
        struct br_frame f = { .type = BR_CLOSE, .session = sid, .len = 0 };
        rx_put(&f, sizeof f);
        microkit_notify(BRIDGE_CH);
    }
    s->used = false;
    pump();                     /* in case frames of its own were holding up the queue */
}

/* write(): one [type][data] record from the program -> one frame to the agent */
long bridge_write(int sid, const uint8_t *rec, size_t len)
{
    struct session *s = &sessions[sid];
    if (len < 1 || len > 1 + BR_MAX_DATA) return -EINVAL;
    if (s->dead) return -EPIPE;
    if (!agent_ready) {                                    /* the VM is still booting */
        hello_waiters |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    long r = send_frame(rec[0], sid, rec + 1, len - 1);
    if (r >= 0) s->started = true;
    return r < 0 ? r : (long)len;
}

/* read(): the next [type][data] record; 0 = session finished */
long bridge_read(int sid, uint8_t *out, size_t len)
{
    struct session *s = &sessions[sid];
    if (s->tail == s->head) {
        if (s->dead) return 0;
        s->waiters |= 1u << PROC_INDEX(cur);
        return POSIX_RETRY;
    }
    uint8_t hdr[5];
    inbox_get(s, hdr, 5);
    uint32_t dlen = hdr[1] | hdr[2] << 8 | hdr[3] << 16 | (uint32_t)hdr[4] << 24;
    size_t n = dlen + 1 <= len ? dlen : (len ? len - 1 : 0);
    out[0] = hdr[0];
    inbox_get(s, out + 1, n);
    s->head += dlen - n;                                   /* whatever did not fit is lost */
    pump();                                                /* it freed room in the inbox */
    return n + 1;
}

/* poll(): POLLIN if there are records or it finished; POLLOUT if there is room towards the VM */
int bridge_poll(int sid, bool want_in, bool want_out, bool wait)
{
    struct session *s = &sessions[sid];
    int ready = 0;
    if (want_in && (s->tail != s->head || s->dead)) ready |= 1;
    if (want_out && agent_ready && rx_free() > sizeof(struct br_frame) + 1024) ready |= 4;
    if (!ready && wait) {
        if (want_in) s->waiters |= 1u << PROC_INDEX(cur);
        if (want_out) {
            rx_waiters |= 1u << PROC_INDEX(cur);
            if (!agent_ready) hello_waiters |= 1u << PROC_INDEX(cur);
        }
    }
    return ready;
}

void bridge_init(void)
{
    serial_queue_init(&rxq, (serial_queue_t *)br_rx_queue, BR_DATA_SIZE, (char *)br_rx_data);
    serial_queue_init(&txq, (serial_queue_t *)br_tx_queue, BR_DATA_SIZE, (char *)br_tx_data);
}
