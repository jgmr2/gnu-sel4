/*
 * netstack: what the FreeBSD stack (F-Stack) expects from its "host", on seL4.
 *
 * F-Stack normally runs as a library inside a Linux process (ff_host_interface.c);
 * here it runs in a Microkit PD with no OS. This file provides:
 *   - memory: a page allocator over the netstack_heap region (ff_mmap and the
 *     mmap that musl's malloc asks for through __sel4_syscall)
 *   - clock (sDDF timer), random numbers (RDRAND), environment, errno
 *   - the minimum to use musl inside the PD: TLS (for errno) and a few syscalls
 *
 * Parts derived from F-Stack's lib/ff_host_interface.c (BSD-2-Clause):
 *   Copyright (c) 2013 Patrick Kelsey. All rights reserved.
 *   Copyright (C) 2017-2021 THL A29 Limited, a Tencent company. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <microkit.h>
#include <sddf/timer/client.h>
#include "ff_host_interface.h"
#include "ff_errno.h"
#include "ff_log.h"
#include "netstack.h"

/* ---------------- memory ---------------- */

uintptr_t netstack_heap;                        /* Microkit region (setvar in meta.py) */
#define HEAP_PAGES (NETSTACK_HEAP_SIZE / 4096)
static uint8_t page_map[HEAP_PAGES / 8];
static size_t page_hint;

#define PG_USED(i) (page_map[(i) / 8] & (1u << ((i) % 8)))

static void pages_mark(size_t first, size_t n, int used)
{
    for (size_t i = first; i < first + n; i++) {
        if (used) page_map[i / 8] |= 1u << (i % 8);
        else page_map[i / 8] &= ~(1u << (i % 8));
    }
}

/* n contiguous zeroed pages (the region starts zeroed and freed pages are cleared) */
static void *pages_alloc(size_t len)
{
    size_t n = (len + 4095) / 4096, run = 0;
    for (size_t k = 0; k < 2 * HEAP_PAGES; k++) {
        size_t i = (page_hint + k) % HEAP_PAGES;
        if (i == 0) run = 0;
        run = PG_USED(i) ? 0 : run + 1;
        if (run == n) {
            size_t first = i + 1 - n;
            pages_mark(first, n, 1);
            page_hint = i + 1;
            return (void *)(netstack_heap + first * 4096);
        }
    }
    return NULL;
}

static int pages_free(void *addr, size_t len)
{
    uintptr_t a = (uintptr_t)addr;
    if (a < netstack_heap || a + len > netstack_heap + NETSTACK_HEAP_SIZE || (a & 4095)) return -EINVAL;
    size_t n = (len + 4095) / 4096;
    memset(addr, 0, n * 4096);
    pages_mark((a - netstack_heap) / 4096, n, 0);
    return 0;
}

void *ff_mmap(void *addr, uint64_t len, int prot, int flags, int fd, uint64_t offset)
{
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    void *p = pages_alloc(len);
    if (!p) panic("netstack: out of memory (ff_mmap of %lu bytes)\n", (unsigned long)len);
    return p;
}

int ff_munmap(void *addr, uint64_t len) { return pages_free(addr, len); }

void *ff_malloc(uint64_t size) { return malloc(size); }
void *ff_calloc(uint64_t number, uint64_t size) { return calloc(number, size); }
void *ff_realloc(void *p, uint64_t size) { return size ? realloc(p, size) : p; }
void ff_free(void *p) { free(p); }

/* ---------------- musl inside the PD ---------------- */

/* musl's malloc asks for memory with mmap; there is no brk (so musl uses mmap) */
long __sel4_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    (void)a3; (void)a5; (void)a6;
    switch (n) {
    case SYS_mmap: {
        if (!(a4 & MAP_ANONYMOUS) || (a4 & MAP_FIXED)) return -ENOMEM;
        void *p = pages_alloc(a2);
        return p ? (long)p : -ENOMEM;
    }
    case SYS_munmap:  return pages_free((void *)a1, a2);
    case SYS_madvise: return 0;
    case SYS_brk:     return 0;
    case SYS_clock_gettime: {
        struct timespec *ts = (struct timespec *)a2;
        uint64_t ns = netstack_now_ns();
        ts->tv_sec = ns / 1000000000ULL;
        ts->tv_nsec = ns % 1000000000ULL;
        return 0;
    }
    case SYS_set_thread_area:                  /* TLS of the only thread (__init_libc asks for it) */
        __asm__ volatile("wrfsbase %0" :: "r"(a1) : "memory");
        return 0;
    case SYS_set_tid_address:
        return 1;
    case SYS_write:
    case SYS_writev:
        return -EBADF;
    }
    return -ENOSYS;
}

/* Initialises musl the way a process start-up would: no environment and a minimal
 * auxiliary vector (AT_RANDOM for malloc, AT_PAGESZ). __init_libc also sets up the
 * thread's TLS (errno). It is internal to musl (hidden visibility), but links statically. */
void __init_libc(char **envp, char *pn);

void netstack_host_init(void)
{
    static uint8_t at_random[16];
    static size_t boot[] = { 0 /* envp: NULL */, AT_RANDOM, 0, AT_PAGESZ, 4096, AT_NULL, 0 };
    static char name[] = "netstack";
    ff_arc4rand(at_random, sizeof at_random, 0);
    boot[2] = (size_t)at_random;
    __init_libc((char **)boot, name);
}

/* ---------------- logging and panic ---------------- */

void netstack_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    microkit_dbg_puts(buf);
}

int ff_log(uint32_t level, uint32_t logtype, const char *format, ...)
{
    (void)logtype;
    if (level > FF_LOG_WARNING) return 0;
    char buf[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    microkit_dbg_puts("netstack: ");
    microkit_dbg_puts(buf);
    return 0;
}

const char *panicstr = NULL;

void panic(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    microkit_dbg_puts("netstack: PANIC: ");
    microkit_dbg_puts(buf);
    for (;;) seL4_Yield();
}

/* ---------------- clock ---------------- */

static struct timespec current_ts;

void ff_clock_gettime(int id, int64_t *sec, long *nsec)
{
    (void)id;
    uint64_t ns = netstack_now_ns();
    *sec = ns / ff_NSEC_PER_SEC;
    *nsec = ns % ff_NSEC_PER_SEC;
}

uint64_t ff_clock_gettime_ns(int id)
{
    (void)id;
    return netstack_now_ns();
}

uint64_t ff_get_tsc_ns(void) { return netstack_now_ns(); }

void ff_get_current_time(int64_t *sec, long *nsec)
{
    if (sec) *sec = current_ts.tv_sec;
    if (nsec) *nsec = current_ts.tv_nsec;
}

void ff_update_current_ts(void)
{
    uint64_t ns = netstack_now_ns();
    current_ts.tv_sec = ns / ff_NSEC_PER_SEC;
    current_ts.tv_nsec = ns % ff_NSEC_PER_SEC;
}

/* ---------------- randomness ---------------- */

static uint64_t xs_state;

static uint64_t rand64(void)
{
    unsigned long long v;
    for (int i = 0; i < 10; i++) {
        unsigned char ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) return v;
    }
    /* no RDRAND: xorshift seeded with the TSC (not cryptographic) */
    if (!xs_state) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        xs_state = ((uint64_t)hi << 32 | lo) | 1;
    }
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return xs_state;
}

void ff_arc4rand(void *ptr, unsigned int len, int reseed)
{
    (void)reseed;
    uint8_t *p = ptr;
    while (len) {
        uint64_t v = rand64();
        unsigned n = len < 8 ? len : 8;
        memcpy(p, &v, n);
        p += n;
        len -= n;
    }
}

uint32_t ff_arc4random(void) { return (uint32_t)rand64(); }

/* ---------------- environment ---------------- */

#define NENV 32
static struct { char name[64], value[64]; } env[NENV];

int ff_setenv(const char *name, const char *value)
{
    for (int i = 0; i < NENV; i++) {
        if (!env[i].name[0] || !strcmp(env[i].name, name)) {
            snprintf(env[i].name, sizeof env[i].name, "%s", name);
            snprintf(env[i].value, sizeof env[i].value, "%s", value);
            return 0;
        }
    }
    return -1;
}

char *ff_getenv(const char *name)
{
    for (int i = 0; i < NENV && env[i].name[0]; i++)
        if (!strcmp(env[i].name, name)) return env[i].value;
    return NULL;
}

/* ---------------- RSS (a single queue: everything is ours) ---------------- */

int ff_in_pcbladdr(uint16_t family, void *faddr, uint16_t fport, void *laddr)
{
    (void)family; (void)faddr; (void)fport; (void)laddr;
    return 0;
}

int ff_rss_check(void *softc, uint32_t saddr, uint32_t daddr, uint16_t sport, uint16_t dport)
{
    (void)softc; (void)saddr; (void)daddr; (void)sport; (void)dport;
    return 1;
}

int ff_rss_tbl_get_portrange(uint32_t saddr, uint32_t daddr, uint16_t sport,
                             uint16_t *rss_first, uint16_t *rss_last, uint16_t **rss_portrange)
{
    (void)saddr; (void)daddr; (void)sport; (void)rss_first; (void)rss_last; (void)rss_portrange;
    return -1;
}

int ff_rss_tbl_set_portrange(uint16_t first, uint16_t last)
{
    (void)first; (void)last;
    return 0;
}

/* ---------------- errno (from lib/ff_host_interface.c) ---------------- */

void ff_os_errno(int error)
{
    switch (error) {
        case ff_EPERM:       errno = EPERM; break;
        case ff_ENOENT:      errno = ENOENT; break;
        case ff_ESRCH:       errno = ESRCH; break;
        case ff_EINTR:       errno = EINTR; break;
        case ff_EIO:         errno = EIO; break;
        case ff_ENXIO:       errno = ENXIO; break;
        case ff_E2BIG:       errno = E2BIG; break;
        case ff_ENOEXEC:     errno = ENOEXEC; break;
        case ff_EBADF:       errno = EBADF; break;
        case ff_ECHILD:      errno = ECHILD; break;
        case ff_EDEADLK:     errno = EDEADLK; break;
        case ff_ENOMEM:      errno = ENOMEM; break;
        case ff_EACCES:      errno = EACCES; break;
        case ff_EFAULT:      errno = EFAULT; break;
        case ff_ENOTBLK:     errno = ENOTBLK; break;
        case ff_EBUSY:       errno = EBUSY; break;
        case ff_EEXIST:      errno = EEXIST; break;
        case ff_EXDEV:       errno = EXDEV; break;
        case ff_ENODEV:      errno = ENODEV; break;
        case ff_ENOTDIR:     errno = ENOTDIR; break;
        case ff_EISDIR:      errno = EISDIR; break;
        case ff_EINVAL:      errno = EINVAL; break;
        case ff_ENFILE:      errno = ENFILE; break;
        case ff_EMFILE:      errno = EMFILE; break;
        case ff_ENOTTY:      errno = ENOTTY; break;
        case ff_ETXTBSY:     errno = ETXTBSY; break;
        case ff_EFBIG:       errno = EFBIG; break;
        case ff_ENOSPC:      errno = ENOSPC; break;
        case ff_ESPIPE:      errno = ESPIPE; break;
        case ff_EROFS:       errno = EROFS; break;
        case ff_EMLINK:      errno = EMLINK; break;
        case ff_EPIPE:       errno = EPIPE; break;
        case ff_EDOM:        errno = EDOM; break;
        case ff_ERANGE:      errno = ERANGE; break;
        case ff_EWOULDBLOCK:     errno = EWOULDBLOCK; break;
        case ff_EINPROGRESS:     errno = EINPROGRESS; break;
        case ff_EALREADY:        errno = EALREADY; break;
        case ff_ENOTSOCK:        errno = ENOTSOCK; break;
        case ff_EDESTADDRREQ:    errno = EDESTADDRREQ; break;
        case ff_EMSGSIZE:        errno = EMSGSIZE; break;
        case ff_EPROTOTYPE:      errno = EPROTOTYPE; break;
        case ff_ENOPROTOOPT:     errno = ENOPROTOOPT; break;
        case ff_EPROTONOSUPPORT: errno = EPROTONOSUPPORT; break;
        case ff_ESOCKTNOSUPPORT: errno = ESOCKTNOSUPPORT; break;
        case ff_ENOTSUP:         errno = ENOTSUP; break;
        case ff_EPFNOSUPPORT:    errno = EPFNOSUPPORT; break;
        case ff_EAFNOSUPPORT:    errno = EAFNOSUPPORT; break;
        case ff_EADDRINUSE:      errno = EADDRINUSE; break;
        case ff_EADDRNOTAVAIL:   errno = EADDRNOTAVAIL; break;
        case ff_ENETDOWN:        errno = ENETDOWN; break;
        case ff_ENETUNREACH:     errno = ENETUNREACH; break;
        case ff_ENETRESET:       errno = ENETRESET; break;
        case ff_ECONNABORTED:    errno = ECONNABORTED; break;
        case ff_ECONNRESET:      errno = ECONNRESET; break;
        case ff_ENOBUFS:         errno = ENOBUFS; break;
        case ff_EISCONN:         errno = EISCONN; break;
        case ff_ENOTCONN:        errno = ENOTCONN; break;
        case ff_ESHUTDOWN:       errno = ESHUTDOWN; break;
        case ff_ETOOMANYREFS:    errno = ETOOMANYREFS; break;
        case ff_ETIMEDOUT:       errno = ETIMEDOUT; break;
        case ff_ECONNREFUSED:    errno = ECONNREFUSED; break;
        case ff_ELOOP:           errno = ELOOP; break;
        case ff_ENAMETOOLONG:    errno = ENAMETOOLONG; break;
        case ff_EHOSTDOWN:       errno = EHOSTDOWN; break;
        case ff_EHOSTUNREACH:    errno = EHOSTUNREACH; break;
        case ff_ENOTEMPTY:       errno = ENOTEMPTY; break;
        case ff_EUSERS:      errno = EUSERS; break;
        case ff_EDQUOT:      errno = EDQUOT; break;
        case ff_ESTALE:      errno = ESTALE; break;
        case ff_EREMOTE:     errno = EREMOTE; break;
        case ff_ENOLCK:      errno = ENOLCK; break;
        case ff_ENOSYS:      errno = ENOSYS; break;
        case ff_EIDRM:       errno = EIDRM; break;
        case ff_ENOMSG:      errno = ENOMSG; break;
        case ff_EOVERFLOW:   errno = EOVERFLOW; break;
        case ff_ECANCELED:   errno = ECANCELED; break;
        case ff_EILSEQ:      errno = EILSEQ; break;
        case ff_EBADMSG:     errno = EBADMSG; break;
        case ff_EMULTIHOP:   errno = EMULTIHOP; break;
        case ff_ENOLINK:     errno = ENOLINK; break;
        case ff_EPROTO:      errno = EPROTO; break;
        default:              errno = error; break;
    }
}

/* ---------------- odds and ends the stack or sDDF expect ---------------- */

/* assert() from the sDDF headers */
void _assert_fail(const char *assertion, const char *file, unsigned int line, const char *function)
{
    panic("assert failed: %s (%s:%u, %s)\n", assertion, file, line, function);
}

/* BSD strtoq (musl does not have it) */
long long strtoq(const char *nptr, char **endptr, int base) { return strtoll(nptr, endptr, base); }

/* The FreeBSD module linker (link_elf.c) reads the "kernel"'s dynamic section:
 * there is none here (only the final DT_NULL entry) */
uint64_t _DYNAMIC[2];
