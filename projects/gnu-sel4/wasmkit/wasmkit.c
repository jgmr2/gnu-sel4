/*
 * wasmkit: what Microkit's runtime and the seL4 kernel do for a component, for components that
 * are WebAssembly modules (TODO item 24, experiment 1). Each protection domain is a module in a
 * Web Worker of its own; all of them share one memory, where the system description (written
 * by wasmkit.js before any worker starts) holds a control block per PD:
 *   - notifications: a bit per channel in the receiver's block, then a wake on its sequence word
 *   - protected procedure calls: the caller takes the callee's call lock, leaves the request,
 *     and waits on the callee's call word; the callee copies the message registers in and the
 *     reply's out, and wakes the caller
 * The waits are memory.atomic.wait32: a worker blocks there as a thread blocks in seL4.
 * Nothing isolates one component's memory from another's yet (one memory): that is what the
 * real kernel, compiled for this, would bring.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <microkit.h>
#include <stddef.h>
#include <stdbool.h>
#include "wasmkit.h"

#define ATOMIC_LOAD(p)      __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define ATOMIC_STORE(p, v)  __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)

static struct wk_pd *me;
static unsigned me_id;
char microkit_name[MICROKIT_PD_NAME_LENGTH];
seL4_Bool microkit_have_signal;
seL4_CPtr microkit_signal_cap;

__attribute__((import_module("env"), import_name("wk_puts"))) void wk_puts(const char *s, uint32_t len);
__attribute__((import_module("env"), import_name("wk_now_ns"))) uint64_t wk_now_ns(void);

static void wake(uint32_t *word) { __builtin_wasm_memory_atomic_notify((int *)word, 1u << 30); }
/* Waits until *word is no longer value. A worker woken from memory.atomic.wait takes tens of
 * microseconds to run again: a short spin first catches the quick answers (most calls) */
#define WK_SPIN 20000
static void wait_while(uint32_t *word, uint32_t value)
{
    for (int i = 0; i < WK_SPIN; i++) if (ATOMIC_LOAD(word) != value) return;
    __builtin_wasm_memory_atomic_wait32((int *)word, (int)value, -1);
}

static void kick(struct wk_pd *pd)
{
    __atomic_add_fetch(&pd->seq, 1, __ATOMIC_SEQ_CST);
    wake(&pd->seq);
}

seL4_Word seL4_GetMR(int i) { return me->mr[i]; }
void seL4_SetMR(int i, seL4_Word v) { me->mr[i] = v; }

uint64_t wasmkit_now_ns(void) { return wk_now_ns(); }

void microkit_dbg_puts(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    wk_puts(s, n);
}
void microkit_dbg_putc(int c)
{
    char ch = (char)c;
    wk_puts(&ch, 1);
}

void microkit_notify(microkit_channel ch)
{
    struct wk_chan t = me->ch[ch];
    if (t.pd == WK_NONE) return;
    struct wk_pd *pd = &WK_SYS->pd[t.pd];
    __atomic_or_fetch(&pd->notes, 1ull << t.their, __ATOMIC_SEQ_CST);
    kick(pd);
}
void microkit_deferred_notify(microkit_channel ch) { microkit_notify(ch); }
void microkit_irq_ack(microkit_channel ch) { (void)ch; }
void microkit_deferred_irq_ack(microkit_channel ch) { (void)ch; }

microkit_msginfo microkit_ppcall(microkit_channel ch, microkit_msginfo msginfo)
{
    struct wk_chan t = me->ch[ch];
    if (t.pd == WK_NONE) return microkit_msginfo_new(~0ull, 0);
    struct wk_pd *pd = &WK_SYS->pd[t.pd];
    uint32_t zero = 0;
    while (!__atomic_compare_exchange_n(&pd->call_lock, &zero, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        wait_while(&pd->call_lock, 1);
        zero = 0;
    }
    pd->call_from = me_id;
    pd->call_ch = t.their;
    pd->call_info = msginfo;
    ATOMIC_STORE(&pd->call, WK_CALL_REQUEST);
    kick(pd);
    while (ATOMIC_LOAD(&pd->call) != WK_CALL_REPLY) wait_while(&pd->call, WK_CALL_REQUEST);
    microkit_msginfo reply = pd->call_info;      /* the callee already copied the registers */
    ATOMIC_STORE(&pd->call, WK_CALL_IDLE);
    ATOMIC_STORE(&pd->call_lock, 0);
    wake(&pd->call_lock);
    return reply;
}

static void serve_call(void)
{
    seL4_Word *mr;                               /* the caller's registers: a PD's or a process's */
    size_t n;
    if (me->call_from >= WK_FROM_PROC) { mr = WK_PROCS[me->call_from - WK_FROM_PROC].mr; n = 16; }
    else { mr = WK_SYS->pd[me->call_from].mr; n = WK_MRS; }
    __builtin_memcpy(me->mr, mr, n * sizeof *mr);
    microkit_msginfo reply = protected(me->call_ch, me->call_info);
    __builtin_memcpy(mr, me->mr, n * sizeof *mr);
    me->call_info = reply;
    ATOMIC_STORE(&me->call, WK_CALL_REPLY);
    wake(&me->call);
}

/* Optional, for a component: its configuration before init (what Microkit's tool patches into
 * an ELF: sDDF's config structures), and a deadline for its waits (absolute, wasmkit_now_ns;
 * 0: none), with what to do when it passes */
__attribute__((weak)) void wasmkit_configure(void) {}
__attribute__((weak)) uint64_t wasmkit_deadline(void) { return 0; }
__attribute__((weak)) void wasmkit_timeout(void) {}

static void wait_events(uint32_t seq)
{
    uint64_t dl = wasmkit_deadline();
    if (!dl) { wait_while(&me->seq, seq); return; }
    uint64_t now = wk_now_ns();
    if (now < dl) __builtin_wasm_memory_atomic_wait32((int *)&me->seq, (int)seq, (long long)(dl - now));
    if (wk_now_ns() >= dl) wasmkit_timeout();
}

/* The worker's entry: this PD's init, then its events for ever */
__attribute__((export_name("wasmkit_run"))) void wasmkit_run(unsigned id)
{
    me_id = id;
    me = &WK_SYS->pd[id];
    __builtin_memcpy(microkit_name, me->name, sizeof microkit_name);
    wasmkit_configure();
    init();
    for (;;) {
        uint32_t seq = ATOMIC_LOAD(&me->seq);
        bool did = false;
        uint64_t notes = __atomic_exchange_n(&me->notes, 0, __ATOMIC_SEQ_CST);
        while (notes) {
            notified(__builtin_ctzll(notes));
            notes &= notes - 1;
            did = true;
        }
        if (ATOMIC_LOAD(&me->call) == WK_CALL_REQUEST) {
            serve_call();
            did = true;
        }
        if (!did) wait_events(seq);
    }
}
