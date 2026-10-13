/*
 * Entry point of a GNU/seL4 program on the browser port (wasmkit, TODO item 24): the page (proc.js)
 * lays out the startup block as Linux does on a 32-bit machine (argc, argv[], NULL, envp[], NULL,
 * auxv[], then the strings) in this buffer, and the runtime starts as on the other ports.
 * The time needs no auxv entries: the page's clock counts nanoseconds since 1970.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdint.h>

_Noreturn void __posix_start(long *sp);
__attribute__((import_module("env"), import_name("wk_startup"))) void wk_startup(long *block, uint32_t size);

static long block[16384] __attribute__((aligned(16)));    /* 64 KiB: arguments and environment */

__attribute__((export_name("_start"))) void _start(void)
{
    wk_startup(block, sizeof block);
    __posix_start(block);
}

/* Signals to a loop with no system calls: not on wasm (they wait for its next call) */
void __posix_async_entry(void) { __builtin_trap(); }
