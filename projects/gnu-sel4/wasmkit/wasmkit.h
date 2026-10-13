/*
 * wasmkit's system description in the shared memory (wasmkit.c, and wasmkit.js writes it: keep
 * the two in step). At WK_CTL_BASE, before every component's own memory.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>
#include <sel4/sel4.h>

#define WK_CTL_BASE 0x10000
#define WK_MAX_PD   16
#define WK_MRS      64
#define WK_NONE     0xff

enum { WK_CALL_IDLE, WK_CALL_REQUEST, WK_CALL_REPLY };

struct wk_chan { uint8_t pd, their; };      /* where a channel leads: the PD, its channel id */

struct wk_pd {                              /* 1024 bytes each */
    uint32_t seq;                           /* bumped on every event: what the PD waits on */
    uint32_t call_lock;                     /* held by the caller of a protected procedure */
    uint64_t notes;                         /* pending notifications: a bit per channel id */
    uint32_t call;                          /* WK_CALL_*: the protected procedure call */
    uint32_t call_from, call_ch;
    uint32_t pad;
    seL4_MessageInfo_t call_info;           /* 16 bytes */
    seL4_Word mr[WK_MRS];                   /* 512 bytes: the message registers */
    struct wk_chan ch[64];                  /* 128 bytes */
    char name[64];
    uint8_t spare[1024 - 48 - 512 - 128 - 64];
};

struct wk_sys {
    uint32_t magic, npd;
    uint8_t pad[1024 - 8];
    struct wk_pd pd[WK_MAX_PD];
};

#define WK_SYS ((struct wk_sys *)WK_CTL_BASE)

/* Processes (posix_server's, experiment 1b): each a worker with a memory of its own, so its
 * calls are made by its JS (proc.js), from a block of its own here. A call's call_from is
 * WK_FROM_PROC + its slot; the server wakes it on wake (POSIX_RETRY) */
#define WK_PROC_BASE 0x18000
#define WK_MAX_PROC  128
#define WK_FROM_PROC 0x100
struct wk_proc {                            /* 256 bytes each */
    uint32_t wake;                          /* bumped by the server: the process may call again */
    uint32_t pad[3];
    seL4_Word mr[16];                       /* its message registers */
    uint8_t spare[256 - 16 - 128];
};
#define WK_PROCS ((struct wk_proc *)WK_PROC_BASE)
_Static_assert(sizeof(struct wk_proc) == 256, "wk_proc layout");
_Static_assert(sizeof(struct wk_pd) == 1024, "wk_pd layout");
