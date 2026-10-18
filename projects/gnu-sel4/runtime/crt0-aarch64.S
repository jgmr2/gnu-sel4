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
 * __posix_async_entry: where posix_server makes a thread run to take a signal while it is in a
 * loop with no system calls (POSIX_SYS_ASYNC in include/sel4posix/protocol.h). sp points at a
 * struct posix_async_frame (pc, sp, spsr, x0-x30 as they were), 16-byte aligned. The FP/SIMD
 * state is saved here (the handlers may use it), the signals delivered, the FP/SIMD state put
 * back; then the frame goes back to the server (POSIX_SYS_ASYNC_RETURN, a seL4_Call by hand:
 * x0 the endpoint, x1 the message info, x2-x5 the first registers, x7 the call), which writes
 * every register from it: no register is free here for a branch back.
 */
#include <sel4posix/procmem.h>
#include <sel4posix/protocol.h>
    .global __posix_async_entry
    .type __posix_async_entry, %function
__posix_async_entry:
    mov x19, sp                     /* the frame (x19 is callee-saved; its value is in the frame) */
    sub sp, sp, #528                /* q0-q31, fpcr, fpsr */
    stp q0, q1, [sp, #0]
    stp q2, q3, [sp, #32]
    stp q4, q5, [sp, #64]
    stp q6, q7, [sp, #96]
    stp q8, q9, [sp, #128]
    stp q10, q11, [sp, #160]
    stp q12, q13, [sp, #192]
    stp q14, q15, [sp, #224]
    stp q16, q17, [sp, #256]
    stp q18, q19, [sp, #288]
    stp q20, q21, [sp, #320]
    stp q22, q23, [sp, #352]
    stp q24, q25, [sp, #384]
    stp q26, q27, [sp, #416]
    stp q28, q29, [sp, #448]
    stp q30, q31, [sp, #480]
    mrs x9, fpcr
    mrs x10, fpsr
    str x9, [sp, #512]              /* (a pair's offset stops at 504) */
    str x10, [sp, #520]
    bl __posix_async_deliver
    ldr x9, [sp, #512]
    ldr x10, [sp, #520]
    msr fpcr, x9
    msr fpsr, x10
    ldp q0, q1, [sp, #0]
    ldp q2, q3, [sp, #32]
    ldp q4, q5, [sp, #64]
    ldp q6, q7, [sp, #96]
    ldp q8, q9, [sp, #128]
    ldp q10, q11, [sp, #160]
    ldp q12, q13, [sp, #192]
    ldp q14, q15, [sp, #224]
    ldp q16, q17, [sp, #256]
    ldp q18, q19, [sp, #288]
    ldp q20, q21, [sp, #320]
    ldp q22, q23, [sp, #352]
    ldp q24, q25, [sp, #384]
    ldp q26, q27, [sp, #416]
    ldp q28, q29, [sp, #448]
    ldp q30, q31, [sp, #480]
    mov sp, x19
    mov x0, #PROC_EP_CAP            /* the server's endpoint */
    mov x1, #POSIX_NUM_MRS          /* message info: label 0, that many registers */
    mov x2, #POSIX_SYS_ASYNC_RETURN
    mov x3, x19                     /* the frame */
    mov x4, #0
    mov x5, #0
    mov x7, #-1                     /* seL4_SysCall */
    svc #0
1:  brk #1                          /* never: the server has put the registers back */
    b 1b
    .section .note.GNU-stack,"",%progbits
