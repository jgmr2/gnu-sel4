/*
 * Memory map of a GNU/seL4 process.
 *
 * Each process runs in a "slot": a thread with its own address space that
 * posix_server creates the first time it is needed (from its untyped memory,
 * see server/mem.c) and reuses afterwards. The regions sit at fixed
 * addresses (so fork() copies memory as is from one slot to another), but
 * their sizes are only virtual limits: pages are created when the process
 * touches them (page fault) and go back to the server on exit, exec and
 * munmap. posix_server sees each page through its direct map (server/mem.c),
 * and each slot's shared buffer through a window of its own (PROC_WIN_*).
 *
 *   0x1000_0000  program image (static ELF linked here), rwx
 *   0x3000_0000  buffer shared with posix_server (syscall data)
 *   0x3010_0000  seL4 IPC buffer
 *   0x4000_0000  heap (brk)
 *   0x6000_0000  stack (grows down from PROC_STACK_BASE + PROC_STACK_SIZE)
 *   0x8000_0000  anonymous mmap
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#define PROC_SLOTS        128               /* processes alive at once (= NPROC; at most 256: the PPC badge's 8 bits, server/microkit_main.c) */

#define PROC_IMG_BASE     0x10000000UL
#define PROC_IMG_SIZE     0x10000000UL      /* 256 MiB */
#ifdef __wasm__
#define PROC_BUF_BASE     0x10000UL         /* the browser port: 1 MiB below the program (linked at 0x110000) */
#else
#define PROC_BUF_BASE     0x30000000UL      /* = POSIX_BUF_SIZE bytes */
#endif
#define PROC_IPCBUF       0x30100000UL      /* 4 KiB, after the shared buffer */
#ifdef __wasm__                             /* in the process's own memory, which grows (it is
                                               contiguous) up to what is used: low, just above the
                                               program (its data and stack, from 0x110000) */
#define PROC_HEAP_BASE    0x00800000UL
#define PROC_HEAP_SIZE    0x00800000UL      /* 8 MiB of brk (musl's malloc maps the rest) */
#else
#define PROC_HEAP_BASE    0x40000000UL
#define PROC_HEAP_SIZE    0x10000000UL      /* 256 MiB */
#endif
#define PROC_STACK_BASE   0x60000000UL
#define PROC_STACK_SIZE   0x00800000UL      /* 8 MiB */
#ifdef __wasm__
#define PROC_MMAP_BASE    0x01000000UL
#define PROC_MMAP_SIZE    0x3f000000UL      /* up to the 1 GiB a process's memory may grow to */
#else
#define PROC_MMAP_BASE    0x80000000UL
#define PROC_MMAP_SIZE    0x40000000UL      /* 1 GiB */
#endif

/* Each slot's window inside posix_server, for the shared and IPC buffers only:
 * address v of the process in slot s is seen at PROC_WIN_BASE + (s << PROC_WIN_SHIFT) + v.
 * aarch64 (40-bit user addresses): 1 GiB per slot, enough for those buffers */
#ifdef __aarch64__
#define PROC_WIN_BASE     0x4000000000UL
#define PROC_WIN_SHIFT    30
#else
#define PROC_WIN_BASE     0x10000000000UL
#define PROC_WIN_SHIFT    32
#endif

/* Our own entries in the auxiliary vector */
#define AT_SEL4_IPCBUF    0x5e14    /* address of the process's seL4 IPC buffer */
#define AT_SEL4_EPOCH     0x5e15    /* UNIX time (s) read from the CMOS clock at boot... */
#define AT_SEL4_TSC0      0x5e16    /* ...and the TSC value at that moment */
#define AT_SEL4_TSCHZ     0x5e17    /* TSC frequency (Hz) */

/* A process's CSpace: a 128-entry CNode with the same numbering as
 * libmicrokit (the runtime uses these capabilities) */
#define PROC_CNODE_BITS   7
#define PROC_NTFN_CAP     1         /* notification: the server wakes the process */
#define PROC_EP_CAP       74        /* the server's endpoint (BASE_ENDPOINT_CAP + POSIX_CHANNEL) */

/* posix_server's root CSpace (64 entries; see <cspace> in meta.py). A cptr
 * (n << 58) | i names entry i of the CNode installed at root slot n. */
#define SRV_ROOT_CNODE_SLOT  1      /* its root CNode (with guard): root[i] = (1 << 58) | i */
#define SRV_POOL_SLOT        2      /* 2..3: the memory manager's untypeds */
#define SRV_NPOOLS           2
#define SRV_POOL_BYTES       (768UL << 20)  /* sum of SRV_POOL_SIZES in meta.py */
#define SRV_ASID_POOL_SLOT   5      /* ASID pool for new address spaces */
#define SRV_TCB_SLOT         6      /* its own TCB (priority authority) */
#define SRV_EP_SLOT          7      /* its endpoint, to mint badged copies */
#define SRV_SCHED_POOL_SLOT  8      /* CNode with PROC_SLOTS scheduling contexts */
#define SRV_FREE_ROOT_SLOT   20     /* 20..63: free for the memory manager's CNodes */
