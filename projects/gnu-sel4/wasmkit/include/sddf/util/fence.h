/*
 * wasmkit: sDDF's memory fences (sddf/util/fence.h) for WebAssembly, which sDDF does not know.
 * The components are Web Workers running in parallel on one shared memory: every fence is a
 * real one (wasm's atomics are sequentially consistent), as sDDF's with SMP support.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>

#define COMPILER_MEMORY_FENCE()   __atomic_signal_fence(__ATOMIC_ACQ_REL)
#define COMPILER_MEMORY_RELEASE() __atomic_signal_fence(__ATOMIC_RELEASE)
#define COMPILER_MEMORY_ACQUIRE() __atomic_signal_fence(__ATOMIC_ACQUIRE)
#define THREAD_MEMORY_FENCE()     __atomic_thread_fence(__ATOMIC_ACQ_REL)
#define THREAD_MEMORY_RELEASE()   __atomic_thread_fence(__ATOMIC_RELEASE)
#define THREAD_MEMORY_ACQUIRE()   __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define rrmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define wwmb() __atomic_thread_fence(__ATOMIC_RELEASE)

static inline uint32_t load_acquire_32(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void store_release_32(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline uint32_t load_relaxed_32(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void store_relaxed_32(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline uint16_t load_acquire_16(const uint16_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static inline void store_release_16(uint16_t *p, uint16_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static inline uint16_t load_relaxed_16(const uint16_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static inline void store_relaxed_16(uint16_t *p, uint16_t v) { __atomic_store_n(p, v, __ATOMIC_RELAXED); }
static inline void fence_seq_cst(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
