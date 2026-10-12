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

/*
 * __posix_async_entry: where posix_server makes a thread run to take a signal while it is in a
 * loop with no system calls (POSIX_SYS_ASYNC in include/sel4posix/protocol.h). rsp points at a
 * struct posix_async_frame with every register as it was, 16-byte aligned, below the red zone.
 * The FPU/SSE state is saved here (the handlers may use it); then the signals are delivered and
 * everything is put back: rip goes into the slot at (old rsp - 136), just below the red zone,
 * and `ret $128` jumps there with rsp as it was.
 */
    .global __posix_async_entry
    .type __posix_async_entry, @function
__posix_async_entry:
    mov %rsp, %rbx                  /* the frame (rbx is callee-saved) */
    sub $512, %rsp                  /* fxsave area: 16-byte aligned like the frame */
    fxsave64 (%rsp)
    cld
    call __posix_async_deliver
    fxrstor64 (%rsp)
    mov %rbx, %rsp
    mov 8(%rsp), %rax               /* the old rsp */
    mov 0(%rsp), %rcx               /* the old rip, into its slot */
    mov %rcx, -136(%rax)
    lea -136(%rax), %rax
    mov %rax, 8(%rsp)               /* rsp to load at the end */
    mov 16(%rsp), %rax              /* rflags */
    push %rax
    popfq
    mov 24(%rsp), %rax              /* nothing below changes the flags */
    mov 32(%rsp), %rbx
    mov 40(%rsp), %rcx
    mov 48(%rsp), %rdx
    mov 56(%rsp), %rsi
    mov 64(%rsp), %rdi
    mov 72(%rsp), %rbp
    mov 80(%rsp), %r8
    mov 88(%rsp), %r9
    mov 96(%rsp), %r10
    mov 104(%rsp), %r11
    mov 112(%rsp), %r12
    mov 120(%rsp), %r13
    mov 128(%rsp), %r14
    mov 136(%rsp), %r15
    mov 8(%rsp), %rsp
    ret $128
    .section .note.GNU-stack,"",@progbits
