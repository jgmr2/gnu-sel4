/*
 * Protocol between the POSIX runtime (in every program) and posix_server.
 *
 * A system call travels as a seL4 protected procedure call (PPC,
 * microkit_ppcall): synchronous, like a Linux syscall.
 *   MR0      = syscall number (Linux x86_64 numbering, the one musl uses)
 *   MR1..MR6 = arguments
 * Reply:
 *   MR0      = return value (negative = -errno, like the Linux kernel)
 *
 * Pointer arguments do not cross: the runtime copies the data into a shared
 * buffer (POSIX_BUF_SIZE bytes) and replaces the pointer with the offset
 * inside the buffer.
 *
 * Blocking calls (read with no data, write with the console full): the
 * server cannot sleep in the middle of a PPC, so it replies POSIX_RETRY.
 * The runtime then waits for a seL4 notification on the same channel
 * (the server sends it once there is data or room) and retries.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#define POSIX_CHANNEL   0           /* channel from the program to the server */
#define POSIX_BUF_SIZE  0x100000    /* 1 MiB: a read or write of up to this much is one call */
#define POSIX_NUM_MRS   7
/* Calls with two paths (rename, symlink...): the second one goes at this offset */
#define POSIX_BUF_PATH2 0x8000

/* In every reply, MR6 carries the process's pending signals (bit s-1 = signal s) */
#define POSIX_MR_SIGNALS 6

/* In a read's reply, MR5 = POSIX_DIRECT when the server already put the data at the program's
 * buffer (the address the runtime gave it): nothing to copy from the shared buffer */
#define POSIX_MR_DIRECT 5
#define POSIX_DIRECT    0x5e14

/* Outside the errno range (-1..-4095): it never reaches the program */
#define POSIX_RETRY     (-0x7000)
/* The process is stopped (job control: SIGSTOP, SIGTSTP...): wait for the server's notification
 * and make the same call again, whatever the call is (the runtime does it in every call). After
 * SIGCONT that gives the result of the call it stopped in, or runs it then */
#define POSIX_STOPPED   (-0x7001)

/* Asynchronous signals (a process in a loop that makes no calls): at start the runtime gives
 * the server, with this call, the address of its in_call counter (MR1: non-zero while it is in a
 * system call, when signals arrive with the reply anyway) and of its entry point (MR2). For a
 * caught signal the server suspends the thread and, if in_call is 0, saves its registers on its
 * stack (below the red zone) as a struct posix_async_frame, makes the entry point run with rsp
 * at that frame, and resumes it; the entry point fetches the signals, runs the handlers and
 * goes back to exactly where the program was (runtime/crt0.S) */
#define POSIX_SYS_ASYNC 1000
#ifndef __ASSEMBLER__
struct posix_async_frame {
    uint64_t rip, rsp, rflags, rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
};
#endif
