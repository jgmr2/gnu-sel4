/*
 * memcpy for posix_server: rep movsb, the CPU's own fast path for copies (ERMS). The one sDDF's
 * library brings (newlib's) copies 8 bytes at a time with non-temporal stores, which also leave
 * what a program reads next out of the cache. Defined here, the library's is not linked in.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    void *r = dst;
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return r;
}
