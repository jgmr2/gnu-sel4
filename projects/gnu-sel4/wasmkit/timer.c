/*
 * wasmkit's timer (TODO item 24, 1b): sDDF's timer protocol for posix_server. GET_TIME is the
 * page's clock in nanoseconds; SET_TIMEOUT a deadline this PD's waits end at (wasmkit_deadline),
 * when the client is notified. One client, one pending timeout, as the server uses it.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <microkit.h>
#include <sddf/timer/protocol.h>
#include "layout.h"

uint64_t wasmkit_now_ns(void);
static uint64_t t0, deadline;

uint64_t wasmkit_deadline(void) { return deadline; }
void wasmkit_timeout(void)
{
    deadline = 0;
    microkit_notify(WK_TIMER_CLIENT);
}

void init(void) { t0 = wasmkit_now_ns(); }
void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo m)
{
    (void)ch;
    uint64_t now = wasmkit_now_ns();
    switch (microkit_msginfo_get_label(m)) {
    case SDDF_TIMER_GET_TIME:
        microkit_mr_set(0, now - t0);
        return microkit_msginfo_new(0, 1);
    case SDDF_TIMER_SET_TIMEOUT:
        deadline = now + microkit_mr_get(0);
        return microkit_msginfo_new(0, 0);
    }
    return microkit_msginfo_new(~0ull, 0);
}
