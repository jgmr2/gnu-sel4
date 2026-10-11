/*
 * Entry point of a GNU/seL4 program. posix_server lays out the stack
 * just like Linux: argc, argv[], NULL, envp[], NULL, auxv[] (see server/proc.c).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
    .text
    .global _start
    .type _start, @function
_start:
    xor %ebp, %ebp          /* marks the end of the frame chain */
    mov %rsp, %rdi          /* argument: pointer to argc */
    and $-16, %rsp          /* ABI alignment */
    call __posix_start
    hlt
    .section .note.GNU-stack,"",@progbits
