/*
 * wasmkit's first client (TODO item 24, experiment 1): a component that talks to fs_ext2 as
 * posix_server does (fs_protocol.h over a protected procedure call, the path and the data in
 * fs_buf), checks a few answers and times them: the bare call, a lookup and a small read, a
 * big file read whole.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <microkit.h>
#include <sel4posix/fs_protocol.h>

#define FS_CH 0
uintptr_t fs_buf;                            /* setvar: shared with fs_ext2 */
uint64_t wasmkit_now_ns(void);

static void puts_(const char *s) { microkit_dbg_puts(s); }
static void putu(uint64_t v)
{
    char b[24];
    int i = sizeof b - 1;
    b[i] = 0;
    do b[--i] = '0' + v % 10; while (v /= 10);
    puts_(b + i);
}
static void putn(const char *label, uint64_t v, const char *unit)
{
    puts_(label);
    putu(v);
    puts_(unit);
}

static long fs_call(long op, uint64_t a1, uint64_t a2, uint64_t a3)
{
    microkit_mr_set(0, op);
    microkit_mr_set(1, a1);
    microkit_mr_set(2, a2);
    microkit_mr_set(3, a3);
    microkit_ppcall(FS_CH, microkit_msginfo_new(0, 4));
    return (long)microkit_mr_get(0);
}
static long lookup(const char *path, uint64_t *size)
{
    char *b = (char *)fs_buf;
    int i = 0;
    while ((b[i] = path[i])) i++;
    long ino = fs_call(FS_OP_LOOKUP, 1, 0, 0);
    if (ino >= 0 && size) *size = microkit_mr_get(2);
    return ino;
}

void init(void)
{
    puts_("bench: fs_ext2 through wasmkit\n");
    uint64_t size;
    long ino = lookup("/etc/os-release", &size);
    if (ino < 0) { putn("bench: lookup /etc/os-release failed: ", -ino, "\n"); return; }
    long n = fs_call(FS_OP_READ, ino, 0, size);
    putn("bench: /etc/os-release, ", n, " bytes:\n");
    char *b = (char *)fs_buf;
    for (long i = 0; i < n && i < 400; i++) microkit_dbg_putc(b[i]);

    enum { N = 2000 };
    uint64_t t0 = wasmkit_now_ns();
    for (int i = 0; i < N; i++) lookup("/", 0);
    uint64_t t1 = wasmkit_now_ns();
    putn("bench: lookup of /: ", (t1 - t0) / N, " ns each (the call itself, nearly)\n");

    t0 = wasmkit_now_ns();
    for (int i = 0; i < N; i++) {
        long f = lookup("/usr/share/vim/vim92/defaults.vim", &size);
        if (f < 0 || fs_call(FS_OP_READ, f, 0, 4096) <= 0) { puts_("bench: read failed\n"); return; }
    }
    t1 = wasmkit_now_ns();
    putn("bench: lookup of a 5-deep path + read 4 KiB: ", (t1 - t0) / N, " ns each\n");

    ino = lookup("/bin/vim", &size);
    if (ino < 0) { puts_("bench: no /bin/vim\n"); return; }
    t0 = wasmkit_now_ns();
    uint64_t done = 0;
    while (done < size) {
        long r = fs_call(FS_OP_READ, ino, done, size - done < FS_BUF_SIZE ? size - done : FS_BUF_SIZE);
        if (r <= 0) { puts_("bench: big read failed\n"); return; }
        done += r;
    }
    t1 = wasmkit_now_ns();
    putn("bench: /bin/vim, ", done >> 10, " KiB read in ");
    putn("", (t1 - t0) / 1000000, " ms: ");
    putn("", done * 1000 / ((t1 - t0) / 1000 + 1) / 1000, " MB/s\n");
    puts_("bench: done\n");
}

void notified(microkit_channel ch) { (void)ch; }
microkit_msginfo protected(microkit_channel ch, microkit_msginfo m) { (void)ch; return m; }
