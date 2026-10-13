/*
 * posix_server's process memory on the browser port (wasmkit, TODO item 24, 1b), in place of
 * mem.c: a process is a WebAssembly module in a worker of its own with a memory of its own,
 * which this server's worker holds too (worker.js). Copies to and from it are the page's
 * (wk_proc_*); each process's shared buffer is a megabyte of the system's memory (proc_bufs),
 * which proc.js copies from and to the process's own around every call. A program's module is
 * read whole into exec_buf and handed to the page.
 *
 * Not here yet: fork (TODO item 24, experiment 2), memfd and MAP_SHARED, CPU accounting.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <microkit.h>
#include <string.h>
#include <sddf/util/printf.h>
#include "server.h"
#include "arch.h"
#include "../wasmkit/wasmkit.h"

#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
IMPORT(wk_proc_exec) long wk_proc_exec(int s, const void *mod, uint32_t len, uint32_t ino,
                                       const char *args, uint32_t args_len, uint32_t argc, uint32_t envc);
IMPORT(wk_proc_end) void wk_proc_end(int s);
IMPORT(wk_proc_write) int wk_proc_write(int s, uint32_t v, const void *src, uint32_t n);   /* src NULL: zeros */
IMPORT(wk_proc_read) int wk_proc_read(int s, uint32_t v, void *dst, uint32_t n);
IMPORT(wk_proc_size) uint32_t wk_proc_size(int s);

uintptr_t proc_bufs;                     /* setvar: PROC_SLOTS x POSIX_BUF_SIZE */
uintptr_t exec_buf;                      /* setvar: EXEC_BUF_SIZE */
#define EXEC_BUF_SIZE 0x1000000

uintptr_t proc_win(int s, uintptr_t vaddr)
{
    return proc_bufs + (uintptr_t)s * POSIX_BUF_SIZE + (vaddr - PROC_BUF_BASE);
}

void slot_signal(int s)
{
    __atomic_add_fetch(&WK_PROCS[s].wake, 1, __ATOMIC_SEQ_CST);
    __builtin_wasm_memory_atomic_notify((int *)&WK_PROCS[s].wake, 1);
}
void slot_suspend(int s) { (void)s; }    /* a worker cannot be paused: its next call stops it */
void slot_resume(int s) { slot_signal(s); }
void slot_halt(int s) { wk_proc_end(s); }
bool slot_make(int s) { (void)s; return true; }

long mem_wasm_exec(int s, long ino, uint64_t size, const char *args, size_t args_len, uint32_t argc, uint32_t envc)
{
    if (size > EXEC_BUF_SIZE) return -E2BIG;
    if (vfs_read_file(ino, 0, (void *)exec_buf, size) != (long)size) return -EIO;
    return wk_proc_exec(s, (void *)exec_buf, size, ino, args, args_len, argc, envc);
}

bool mem_copy_in(int s, uintptr_t v, const void *src, size_t n) { return wk_proc_write(s, v, src, n); }
bool mem_copy_user(int s, uintptr_t v, const void *src, size_t n) { return wk_proc_write(s, v, src, n); }
bool mem_copy_from_user(int s, uintptr_t v, void *dst, size_t n) { return wk_proc_read(s, v, dst, n); }
bool mem_has_page(int s, uintptr_t v) { return v < wk_proc_size(s); }
uint8_t *mem_page_ptr(int s, uintptr_t v)
{
    if (v >= PROC_BUF_BASE && v < PROC_BUF_BASE + POSIX_BUF_SIZE) return (uint8_t *)proc_win(s, v);
    return NULL;                         /* the rest is in another memory: copies only */
}
bool mem_buf_ensure(int s, size_t len) { (void)s; (void)len; return true; }
bool mem_ensure(int s, uintptr_t vaddr, size_t len) { return vaddr + len <= wk_proc_size(s); }
void mem_free_range(int s, uintptr_t vaddr, size_t len) { (void)s; (void)vaddr; (void)len; }
void mem_free_all(int s) { wk_proc_end(s); }
bool mem_fork(int cs, int ps) { (void)cs; (void)ps; return false; }
bool mem_fault(int s, uintptr_t addr) { (void)s; (void)addr; return false; }
void mem_sync_code(int s, uintptr_t v) { (void)s; (void)v; }
uint32_t mem_slot_pages(int s) { return wk_proc_size(s) / 4096; }
void mem_slot_set_prio(int s, int prio) { (void)s; (void)prio; }
uint64_t mem_slot_consumed_us(int s) { (void)s; return 0; }

uint64_t mem_total(void) { return 1ull << 30; }
uint64_t mem_free(void)
{
    uint64_t used = 0;
    for (int s = 0; s < PROC_SLOTS; s++) used += wk_proc_size(s);
    return used < mem_total() ? mem_total() - used : 0;
}
void mem_init(void) {}

long mem_shm_new(void) { return -ENOSYS; }
void mem_shm_ref(int id) { (void)id; }
void mem_shm_unref(int id) { (void)id; }
uint64_t mem_shm_size(int id) { (void)id; return 0; }
long mem_shm_truncate(int id, uint64_t size) { (void)id; (void)size; return -ENOSYS; }
long mem_shm_rw(int id, uint64_t off, void *buf, size_t len, bool write)
{
    (void)id; (void)off; (void)buf; (void)len; (void)write;
    return -ENOSYS;
}
long mem_shm_map(int s, int id, uintptr_t vaddr, size_t len, uint64_t off, bool write)
{
    (void)s; (void)id; (void)vaddr; (void)len; (void)off; (void)write;
    return -ENOSYS;
}

size_t mem_maps_text(int s, const char *exe, char *out, size_t max)
{
    return sddf_snprintf(out, max, "00000000-%08x rw-p 00000000 00:00 0 %s\n", wk_proc_size(s), exe);
}
