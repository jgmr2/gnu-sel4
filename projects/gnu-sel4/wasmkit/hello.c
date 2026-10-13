/*
 * wasmkit's first process (TODO item 24, 1b), as /sbin/init: no libc yet (experiment 2), the
 * POSIX protocol by hand, as runtime/syscall.c speaks it. It writes to the console, reads a file
 * through posix_server and fs_ext2, asks the time, sleeps, and exits.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdint.h>
#include <sel4posix/procmem.h>
#include <sel4posix/protocol.h>

#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
IMPORT(wk_call) int64_t wk_call(int64_t n, int64_t a1, int64_t a2, int64_t a3, int64_t a4, int64_t a5, int64_t a6);
IMPORT(wk_wait) void wk_wait(void);
IMPORT(wk_now_ns) uint64_t wk_now_ns(void);

enum { NR_read = 0, NR_write = 1, NR_close = 3, NR_nanosleep = 35, NR_getpid = 39, NR_exit_group = 231,
       NR_openat = 257 };
#define BUF ((char *)PROC_BUF_BASE)

static long call(long n, long a1, long a2, long a3, long a4)
{
    for (;;) {
        long r = wk_call(n, a1, a2, a3, a4, 0, 0);
        if (r != POSIX_RETRY) return r;
        wk_wait();                       /* until the server wakes it: then the call again */
    }
}
static long len_(const char *s) { long n = 0; while (s[n]) n++; return n; }
static long write_(const char *s, long n)
{
    for (long i = 0; i < n; i++) BUF[i] = s[i];
    return call(NR_write, 1, 0, n, 0);
}
static void puts_(const char *s) { write_(s, len_(s)); }
static void putu(uint64_t v)
{
    char b[24];
    int i = sizeof b;
    do b[--i] = '0' + v % 10; while (v /= 10);
    write_(b + i, sizeof b - i);
}

__attribute__((export_name("_start"))) void _start(void)
{
    puts_("\r\nhello from a WebAssembly process on posix_server (wasmkit)\r\n");
    puts_("pid ");
    putu(call(NR_getpid, 0, 0, 0, 0));
    puts_("\r\n");

    const char *path = "/etc/os-release";
    for (long i = 0; i <= len_(path); i++) BUF[i] = path[i];
    long fd = call(NR_openat, -100, 0, 0, 0);
    if (fd < 0) { puts_("open failed\r\n"); }
    else {
        long n = call(NR_read, fd, 0, 4096, 0);
        if (n > 0) call(NR_write, 1, 0, n, 0);   /* what read left in the buffer, as it is */
        call(NR_close, fd, 0, 0, 0);
    }

    uint64_t t0 = wk_now_ns();
    enum { N = 1000 };
    for (int i = 0; i < N; i++) call(NR_getpid, 0, 0, 0, 0);
    puts_("a system call: ");
    putu((wk_now_ns() - t0) / N);
    puts_(" ns\r\nsleeping 1 s (the timer)... ");
    t0 = wk_now_ns();
    for (uint64_t target = t0 + 1000000000, now; (now = wk_now_ns()) < target; )   /* as the runtime */
        if (wk_call(NR_nanosleep, target - now, 0, 0, 0, 0, 0) == POSIX_RETRY) wk_wait();
    putu((wk_now_ns() - t0) / 1000000);
    puts_(" ms\r\nbye\r\n");
    call(NR_exit_group, 0, 0, 0, 0);
    for (;;) wk_wait();
}
