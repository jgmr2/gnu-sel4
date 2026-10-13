/*
 * What the runtime does differently on each architecture: the cycle counter that keeps time, the
 * processor's random number generator, and the thread pointer (TLS).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>

#if defined(__x86_64__)

static inline uint64_t arch_counter(void)          /* the TSC */
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static inline int arch_hwrand(uint64_t *v)          /* RDRAND: 1 if it gave a value */
{
    unsigned long long x;
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(x), "=qm"(ok));
    *v = x;
    return ok;
}

/* seL4 x86_64 is configured with FSGSBASE_INST: wrfsbase in user mode */
static inline void arch_set_tp(uintptr_t tp) { __asm__ volatile("wrfsbase %0" :: "r"(tp) : "memory"); }
static inline uintptr_t arch_get_tp(void) { uintptr_t tp; __asm__ volatile("rdfsbase %0" : "=r"(tp)); return tp; }

#elif defined(__aarch64__)

/* The physical counter: seL4 lets user mode read it (CONFIG_EXPORT_PCNT_USER), not the virtual
 * one. Its frequency is in CNTFRQ_EL0 */
static inline uint64_t arch_counter(void)
{
    uint64_t c;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(c));
    return c;
}

static inline uint64_t arch_counter_hz(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

/* No RNDR without knowing the CPU has it (its ID register is not readable from user mode):
 * the caller falls back to its counter mix. TODO: entropy from the server (virtio-rng) */
static inline int arch_hwrand(uint64_t *v) { (void)v; return 0; }

static inline void arch_set_tp(uintptr_t tp) { __asm__ volatile("msr tpidr_el0, %0" :: "r"(tp) : "memory"); }
static inline uintptr_t arch_get_tp(void) { uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp)); return tp; }

#else
#error "runtime/arch.h: unsupported architecture"
#endif
