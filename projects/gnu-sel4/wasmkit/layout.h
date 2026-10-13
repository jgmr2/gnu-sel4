/*
 * The wasmkit system's shared memory, by hand for now (what meta.py does for Microkit). The
 * regions JS fills or tells components about are in system.json too: keep both in step.
 *
 *   0x00010000  wasmkit's control blocks (wasmkit.h), 0x18000 the processes' blocks
 *   0x01000000  fs_ext2          0x02000000  console     0x02400000  timer
 *   0x03000000  posix_server (16 MiB)
 *   0x04000000  fs_buf (1 MiB)   0x04200000  the serial queues and their data
 *   0x05000000  exec_buf (16 MiB): a program's module, read by the server for the page
 *   0x06000000  proc_bufs (128 x 1 MiB): each process's shared buffer, as the server sees it
 *   0x0e000000  the RAM disk (fs_ext2)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#define WK_SERIAL_RX_QUEUE 0x04200000
#define WK_SERIAL_RX_DATA  0x04201000
#define WK_SERIAL_RX_SIZE  0x1000
#define WK_SERIAL_TX_QUEUE 0x04202000
#define WK_SERIAL_TX_DATA  0x04210000
#define WK_SERIAL_TX_SIZE  0x10000
#define WK_CONSOLE_IN      0x04220000      /* typed bytes: the page writes, the console reads */
#define WK_CONSOLE_IN_SIZE 0x1000          /* a ring: u32 head, u32 tail, then the bytes */

/* Channels */
#define WK_SERVER_SERIAL_TX 30             /* posix_server -> console: there is output */
#define WK_SERVER_SERIAL_RX 31             /* console -> posix_server: there is input */
#define WK_SERVER_TIMER     32             /* posix_server <-> timer */
#define WK_CONSOLE_TX       0
#define WK_CONSOLE_RX       1
#define WK_CONSOLE_KEYS     2              /* the page -> console: keys in WK_CONSOLE_IN */
#define WK_TIMER_CLIENT     0
