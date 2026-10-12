/*
 * posix_server's clock: at boot it reads the time from the PC's CMOS clock
 * (ports 0x70/0x71) and the TSC frequency. Programs receive that data in
 * the auxiliary vector and work out the time without IPC (runtime/syscall.c);
 * fs_ext2 receives it to timestamp files.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <os/sddf.h>
#include <sddf/timer/client.h>
#include <sddf/timer/config.h>
#include "server.h"

/* sDDF timer client (HPET): poll() and nanosleep() timeouts */
__attribute__((__section__(".timer_client_config"))) timer_client_config_t timer_config;

#define RTC_IOPORT 0       /* id of the RTC I/O port in meta.py */

uint64_t clock_epoch, clock_tsc0, clock_tsc_hz;

uint64_t clock_rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static uint8_t cmos(uint8_t reg)
{
    microkit_x86_ioport_write_8(RTC_IOPORT, 0x70, reg);
    return microkit_x86_ioport_read_8(RTC_IOPORT, 0x71);
}

static uint64_t days_from_civil(int y, unsigned m, unsigned d)
{
    /* days since 1970-01-01 (H. Hinnant's algorithm) */
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint64_t)(era * 146097 + (int)doe - 719468);
}

static uint64_t read_rtc(void)
{
    while (cmos(0x0a) & 0x80) ;                       /* wait until it is not updating */
    unsigned sec = cmos(0), min = cmos(2), hour = cmos(4), day = cmos(7), mon = cmos(8), year = cmos(9);
    uint8_t b = cmos(0x0b);
    if (!(b & 0x04)) {                                /* BCD -> binary */
#define BCD(x) (((x) & 0x0f) + ((x) >> 4) * 10)
        sec = BCD(sec); min = BCD(min); hour = BCD(hour & 0x7f) | (hour & 0x80);
        day = BCD(day); mon = BCD(mon); year = BCD(year);
    }
    if (!(b & 0x02) && (hour & 0x80)) hour = ((hour & 0x7f) + 12) % 24;   /* 12-hour mode */
    hour &= 0x7f;
    return days_from_civil(2000 + year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

static uint64_t tsc_frequency(void)
{
    uint32_t a, b, c, d;
    /* KVM: hypervisor leaf with the TSC frequency in kHz */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x40000000), "c"(0));
    if (a >= 0x40000010) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x40000010), "c"(0));
        if (a) return (uint64_t)a * 1000;
    }
    /* Intel: crystal x ratio (leaf 0x15), or base frequency (leaf 0x16) */
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    uint32_t max = a;
    if (max >= 0x15) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x15), "c"(0));
        if (a && b && c) return (uint64_t)c * b / a;
    }
    if (max >= 0x16) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x16), "c"(0));
        if (a) return (uint64_t)(a & 0xffff) * 1000000;
    }
    /* Last resort: measure against the RTC's seconds tick */
    unsigned s0 = cmos(0);
    while (cmos(0) == s0) ;
    uint64_t t0 = clock_rdtsc();
    s0 = cmos(0);
    while (cmos(0) == s0) ;
    return clock_rdtsc() - t0;
}

void clock_init(void)
{
    clock_tsc_hz = tsc_frequency();
    clock_epoch = read_rtc();
    clock_tsc0 = clock_rdtsc();
}

uint64_t clock_now(void)
{
    return clock_epoch + (clock_rdtsc() - clock_tsc0) / clock_tsc_hz;
}

/* ---------------- deadlines ---------------- */

uint64_t timer_now_ns(void)
{
    return sddf_timer_time_now(timer_config.driver_id);
}

/* Programs the timer for the nearest deadline (poll/sleep or setitimer) */
void timer_rearm(void)
{
    uint64_t now = sddf_timer_time_now(timer_config.driver_id), next = 0;
    for (int i = 0; i < NPROC; i++) {
        if (procs[i].state != P_RUN) continue;
        uint64_t d = procs[i].deadline, t = procs[i].itimer_next;
        if (d && (!next || d < next)) next = d;
        if (t && (!next || t < next)) next = t;
    }
    uint64_t tf = vfs_timerfd_next();
    if (tf && (!next || tf < next)) next = tf;
    if (fs_flush_at && (!next || fs_flush_at < next)) next = fs_flush_at;     /* write-back */
    if (next) sddf_timer_set_timeout(timer_config.driver_id, next > now ? next - now : 1);
}

void timer_wait(struct proc *p, uint64_t ns)
{
    p->deadline = sddf_timer_time_now(timer_config.driver_id) + (ns ? ns : 1);
    timer_rearm();
}

bool timer_notified(unsigned ch)
{
    if (ch != timer_config.driver_id) return false;
    uint64_t now = sddf_timer_time_now(timer_config.driver_id);
    if (fs_flush_at && fs_flush_at <= now) {           /* fs_ext2's dirty blocks to the disk */
        fs_flush_at = 0;
        fs_call(FS_OP_SYNC, 0, 0, 0);
    }
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &procs[i];
        if (p->state != P_RUN) continue;
        if (p->deadline && p->deadline <= now) {
            p->deadline = 0;
            proc_wake(p);
        }
        if (p->itimer_next && p->itimer_next <= now) {            /* setitimer: SIGALRM */
            p->itimer_next = p->itimer_interval ? now + p->itimer_interval : 0;
            proc_signal(p, SIGALRM);
        }
    }
    vfs_timerfd_fire(now);
    timer_rearm();
    return true;
}
