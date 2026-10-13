/*
 * Entry point of a GNU/seL4 program on aarch64 (runtime/crt0.S is x86_64's). posix_server lays
 * out the stack just like Linux: argc, argv[], NULL, envp[], NULL, auxv[] (server/proc.c).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
    .text
    .global _start
    .type _start, %function
_start:
    mov x29, #0             /* the end of the frame chain */
    mov x30, #0
    mov x0, sp              /* argument: pointer to argc */
    and sp, x0, #-16        /* ABI alignment (it already is) */
    bl __posix_start
1:  brk #0
    b 1b

/*
 * __posix_async_entry: signals to a thread in a loop with no system calls (POSIX_SYS_ASYNC).
 * Not on aarch64 yet (TODO item 16, milestone 2): posix_server does not send them here; if it
 * ever runs, it stops the thread visibly instead of corrupting its registers.
 */
    .global __posix_async_entry
    .type __posix_async_entry, %function
__posix_async_entry:
    brk #1
    b __posix_async_entry
    .section .note.GNU-stack,"",%progbits
