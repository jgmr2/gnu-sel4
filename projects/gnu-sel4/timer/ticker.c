/*
 * timer_ticker: the timer driver's heartbeat, from the seL4 kernel's own timer.
 *
 * On the real laptop the HPET almost stops while the CPU idles (Linux does not use it there
 * either), so its alarm cannot be trusted to wake anything up. The kernel's timer (the local
 * APIC, which keeps running in idle states on CPUs with ARAT) is not available to user space,
 * but the MCS scheduler uses it to refill budgets: this PD has 100 us of budget every 1 ms
 * (meta.py; 20 us is the kernel's minimum, which it never refills), and it yields the rest at
 * every turn, so it runs once per millisecond. Each time it reads the TSC and, if the earliest
 * deadline the driver published (timer_next) has passed, notifies the driver, which then does
 * what an HPET interrupt would have done. About 0.2 % of the CPU; deadlines at ~1 ms precision.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdint.h>
#include <microkit.h>
#include <sddf/util/arch_timestamp_counter.h>   /* sddf_read_counter */
#include <sddf/timer/timer_common.h>

#define DRIVER_CH 0                     /* to timer_driver (TICK_CH there) */

uintptr_t timer_next;                   /* the driver's earliest deadline, ns (UINT64_MAX: none) */

void init(void)
{
    volatile uint64_t *next = (volatile uint64_t *)timer_next;
    /* The TSC frequency the driver measured (timer_next[1], valid once [2] is set); 0 when there
     * is no usable TSC (then the HPET is the clock and the alarm, and there is nothing to do) */
    while (!__atomic_load_n(&next[2], __ATOMIC_ACQUIRE)) seL4_Yield();
    uint64_t freq = next[1];
    if (!freq) return;
    microkit_dbg_puts("timer_ticker: running (the kernel's timer drives the deadlines)\n");
    uint64_t told = UINT64_MAX;         /* the deadline we already rang for */
    uint64_t second = sddf_read_counter(), beats = 0;
    for (;;) {
        uint64_t t = sddf_read_counter();
        uint64_t deadline = *next;
        if (deadline != UINT64_MAX && deadline != told && ticks_to_ns(t, freq) >= deadline) {
            told = deadline;
            microkit_notify(DRIVER_CH);
        }
        beats++;
        if (t - second > 10 * freq) {   /* every 10 s: how often the kernel ran us (debug log) */
            microkit_dbg_puts("timer_ticker: beats in 10 s: ");
            char b[16]; int n = 0; uint64_t w = beats; do b[n++] = '0' + w % 10; while (w /= 10);
            while (n) microkit_dbg_putc(b[--n]);
            microkit_dbg_puts("\n");
            beats = 0; second = t;
        }
        seL4_Yield();                   /* MCS: the rest of this budget goes; back at the next refill */
    }
}

void notified(microkit_channel ch)
{
    (void)ch;
}
