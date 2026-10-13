/*
 * wasmkit's console (TODO item 24, 1b): what sDDF's serial driver and virtualisers are for
 * posix_server, with the page's terminal (xterm.js) as the device. Output: the server's
 * transmit queue to the page (wk_term_write). Input: the keys the page leaves in WK_CONSOLE_IN
 * (a ring) to the server's receive queue.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <microkit.h>
#include <sddf/serial/queue.h>
#include "layout.h"

__attribute__((import_module("env"), import_name("wk_term_write"))) void wk_term_write(const char *s, uint32_t len);

static serial_queue_handle_t rx, tx;

void init(void)
{
    serial_queue_init(&rx, (serial_queue_t *)WK_SERIAL_RX_QUEUE, WK_SERIAL_RX_SIZE, (char *)WK_SERIAL_RX_DATA);
    serial_queue_init(&tx, (serial_queue_t *)WK_SERIAL_TX_QUEUE, WK_SERIAL_TX_SIZE, (char *)WK_SERIAL_TX_DATA);
}

static void output(void)
{
    static char out[4096];
    uint32_t n = 0;
    char c;
    while (!serial_dequeue(&tx, &c)) {
        out[n++] = c;
        if (n == sizeof out) { wk_term_write(out, n); n = 0; }
    }
    if (n) wk_term_write(out, n);
    if (serial_require_consumer_signal(&tx)) {   /* the server waits for room */
        serial_cancel_consumer_signal(&tx);
        microkit_notify(WK_CONSOLE_TX);
    }
}

static void input(void)
{
    volatile uint32_t *ring = (volatile uint32_t *)WK_CONSOLE_IN;
    uint8_t *bytes = (uint8_t *)(WK_CONSOLE_IN + 8);
    uint32_t head = ring[0], tail = __atomic_load_n(&ring[1], __ATOMIC_ACQUIRE);
    bool any = false;
    while (head != tail && !serial_queue_full(&rx, rx.queue->tail)) {
        serial_enqueue(&rx, bytes[head % (WK_CONSOLE_IN_SIZE - 8)]);
        head++;
        any = true;
    }
    __atomic_store_n(&ring[0], head, __ATOMIC_RELEASE);
    if (any) microkit_notify(WK_CONSOLE_RX);
}

void notified(microkit_channel ch)
{
    if (ch == WK_CONSOLE_TX) output();
    else if (ch == WK_CONSOLE_KEYS || ch == WK_CONSOLE_RX) input();
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo m) { (void)ch; return m; }
