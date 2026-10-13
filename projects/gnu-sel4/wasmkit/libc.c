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
