/*
 * wasmkit: Microkit's API for components running as WebAssembly modules, one Web Worker each,
 * all in one shared memory (TODO item 24). The calls are what libmicrokit offers; wasmkit.c
 * implements them with the shared memory's atomics instead of seL4's system calls.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <sel4/sel4.h>

typedef unsigned int microkit_channel;
typedef unsigned int microkit_child;
typedef seL4_MessageInfo_t microkit_msginfo;

#define MICROKIT_MAX_CHANNELS 62
#define MICROKIT_PD_NAME_LENGTH 64
#define BASE_OUTPUT_NOTIFICATION_CAP 10

extern char microkit_name[MICROKIT_PD_NAME_LENGTH];
extern seL4_Bool microkit_have_signal;
extern seL4_CPtr microkit_signal_cap;

/* What the component provides */
void init(void);
void notified(microkit_channel ch);
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo);

void microkit_notify(microkit_channel ch);
void microkit_deferred_notify(microkit_channel ch);
void microkit_irq_ack(microkit_channel ch);
void microkit_deferred_irq_ack(microkit_channel ch);
microkit_msginfo microkit_ppcall(microkit_channel ch, microkit_msginfo msginfo);
void microkit_dbg_puts(const char *s);
void microkit_dbg_putc(int c);

static inline microkit_msginfo microkit_msginfo_new(seL4_Word label, seL4_Uint16 count)
{
    return (microkit_msginfo){ label, count };
}
static inline seL4_Word microkit_msginfo_get_label(microkit_msginfo m) { return m.label; }
static inline seL4_Word microkit_msginfo_get_count(microkit_msginfo m) { return m.count; }
static inline void microkit_mr_set(seL4_Uint8 mr, seL4_Word value) { seL4_SetMR(mr, value); }
static inline seL4_Word microkit_mr_get(seL4_Uint8 mr) { return seL4_GetMR(mr); }
