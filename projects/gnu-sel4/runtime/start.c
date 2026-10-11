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
#include <sel4/sel4.h>
#include <sel4posix/procmem.h>

seL4_IPCBuffer *__sel4_ipc_buffer;

int __libc_start_main(int (*)(int, char **, char **), int, char **,
                      void (*)(void), void (*)(void), void (*)(void));
int main(int, char **, char **);
void __posix_runtime_init(void);
extern uint64_t __posix_epoch, __posix_tsc0, __posix_tsc_hz;

_Noreturn void __posix_start(long *sp)
{
    int argc = sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    while (*envp) envp++;
    for (Elf64_auxv_t *aux = (Elf64_auxv_t *)(envp + 1); aux->a_type != AT_NULL; aux++)
        switch (aux->a_type) {
        case AT_SEL4_IPCBUF: __sel4_ipc_buffer = (seL4_IPCBuffer *)aux->a_un.a_val; break;
        case AT_SEL4_EPOCH:  __posix_epoch = aux->a_un.a_val; break;
        case AT_SEL4_TSC0:   __posix_tsc0 = aux->a_un.a_val; break;
        case AT_SEL4_TSCHZ:  if (aux->a_un.a_val) __posix_tsc_hz = aux->a_un.a_val; break;
        }
    __posix_runtime_init();
    __libc_start_main(main, argc, argv, 0, 0, 0);
    for (;;) ;
}
