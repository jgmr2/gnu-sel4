/*
 * The runtime on the browser port (wasmkit, TODO item 24): what seL4's IPC is elsewhere. A call
 * to posix_server is the process's JS (proc.js: wk_ipc), given the message registers' address:
 * it reads the request there and writes the reply back; waiting for the server's wake-up is
 * wk_wait. The rest of seL4's names the runtime uses, as no-ops.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>

typedef uint64_t seL4_Word;
#define BASE_ENDPOINT_CAP 0
#define seL4_MessageInfo_new(label, caps, extra, length) 0

__attribute__((import_module("env"), import_name("wk_ipc"))) void wk_ipc(seL4_Word *mrs);
__attribute__((import_module("env"), import_name("wk_wait"))) void wk_wait(void);

static seL4_Word wasm_mrs[8];
#define mr_set(i, v) (wasm_mrs[i] = (seL4_Word)(v))
#define mr_get(i)    (wasm_mrs[i])
#define seL4_Call(cap, info) wk_ipc(wasm_mrs)
#define seL4_Wait(cap, badge) ((void)(badge), wk_wait())
#define seL4_Yield() ((void)0)

/* The process's memory grows to hold end (brk, mmap): 1 if it does */
static inline int wasm_memory_ensure(uintptr_t end)
{
    uintptr_t have = __builtin_wasm_memory_size(0) * 65536;
    return end <= have || __builtin_wasm_memory_grow(0, (end - have + 65535) / 65536) != (uintptr_t)-1;
}
