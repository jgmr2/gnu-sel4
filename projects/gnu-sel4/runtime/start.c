/*
 * Start-up of a GNU/seL4 program: finds this slot's seL4 IPC buffer in the
 * auxiliary vector and enters musl (__libc_start_main), which calls the
 * program's main().
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stddef.h>
#include <stdint.h>
#include <elf.h>
#include <sel4posix/procmem.h>

#ifdef __wasm__
/* The browser port: 32-bit words in the startup block; clang names main by its parameters on
 * wasm32 (main(void): __main_void, main(argc, argv): __main_argc_argv, with envp: main) */
typedef Elf32_auxv_t auxv_t;
__attribute__((weak)) int __main_argc_argv(int, char **);
__attribute__((weak)) int __main_void(void);
__attribute__((weak)) int main(int, char **, char **);
static int wasm_main(int argc, char **argv, char **envp)
{
    if (__main_argc_argv) return __main_argc_argv(argc, argv);
    if (__main_void) return __main_void();
    return main(argc, argv, envp);
}
#define main wasm_main
#else
#include <sel4/sel4.h>
typedef Elf64_auxv_t auxv_t;
seL4_IPCBuffer *__sel4_ipc_buffer;
int main(int, char **, char **);
#endif

int __libc_start_main(int (*)(int, char **, char **), int, char **,
                      void (*)(void), void (*)(void), void (*)(void));
void __posix_runtime_init(void);
extern uint64_t __posix_epoch, __posix_tsc0, __posix_tsc_hz;

_Noreturn void __posix_start(long *sp)
{
    int argc = sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    while (*envp) envp++;
    for (auxv_t *aux = (auxv_t *)(envp + 1); aux->a_type != AT_NULL; aux++)
        switch (aux->a_type) {
#ifndef __wasm__
        case AT_SEL4_IPCBUF: __sel4_ipc_buffer = (seL4_IPCBuffer *)aux->a_un.a_val; break;
#endif
        case AT_SEL4_EPOCH:  __posix_epoch = aux->a_un.a_val; break;
        case AT_SEL4_TSC0:   __posix_tsc0 = aux->a_un.a_val; break;
        case AT_SEL4_TSCHZ:  if (aux->a_un.a_val) __posix_tsc_hz = aux->a_un.a_val; break;
        }
    __posix_runtime_init();
    __libc_start_main(main, argc, argv, 0, 0, 0);
    for (;;) ;
}
