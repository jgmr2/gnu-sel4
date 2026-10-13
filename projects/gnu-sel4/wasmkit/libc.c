/*
 * wasmkit: the string functions the components need (sDDF's custom libc has them in assembly
 * per architecture). memcpy, memmove and memset become wasm's memory.copy and memory.fill
 * (-mbulk-memory).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n) { __builtin_memcpy(d, s, n); return d; }
void *memmove(void *d, const void *s, size_t n) { __builtin_memmove(d, s, n); return d; }
void *memset(void *d, int c, size_t n) { __builtin_memset(d, c, n); return d; }

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return p - s;
}
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}

/* compiler-rt's 128-bit helpers, which clang calls on wasm32 (posix_server's wait masks are
 * __int128): shifts and the product */
typedef unsigned __int128 u128;
typedef __int128 i128;
typedef unsigned long long u64;
u128 __ashlti3(u128 a, int b)
{
    u64 lo = (u64)a, hi = (u64)(a >> 64);
    if (b == 0) return a;
    if (b >= 64) return (u128)(lo << (b - 64)) << 64;
    return (u128)(hi << b | lo >> (64 - b)) << 64 | (u128)(lo << b);
}
u128 __lshrti3(u128 a, int b)
{
    u64 lo = (u64)a, hi = (u64)(a >> 64);
    if (b == 0) return a;
    if (b >= 64) return (u128)(hi >> (b - 64));
    return (u128)(hi >> b) << 64 | (u128)(lo >> b | hi << (64 - b));
}
i128 __ashrti3(i128 a, int b)
{
    long long hi = (long long)(a >> 64);
    u64 lo = (u64)a;
    if (b == 0) return a;
    if (b >= 64) return (i128)(hi >> (b - 64 > 63 ? 63 : b - 64));
    return (i128)((u128)(u64)(hi >> b) << 64 | (u128)(lo >> b | (u64)hi << (64 - b)));
}
u128 __multi3(u128 a, u128 b)
{
    u64 a0 = (u64)a, a1 = (u64)(a >> 64), b0 = (u64)b, b1 = (u64)(b >> 64);
    u64 a0l = a0 & 0xffffffff, a0h = a0 >> 32, b0l = b0 & 0xffffffff, b0h = b0 >> 32;
    u64 ll = a0l * b0l, lh = a0l * b0h, hl = a0h * b0l, hh = a0h * b0h;
    u64 mid = (ll >> 32) + (lh & 0xffffffff) + (hl & 0xffffffff);
    u64 lo = (ll & 0xffffffff) | mid << 32;
    u64 hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32) + a0 * b1 + a1 * b0;
    return (u128)hi << 64 | lo;
}
