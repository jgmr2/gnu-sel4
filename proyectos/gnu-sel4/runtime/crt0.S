/*
 * Punto de entrada de un programa de GNU/seL4. posix_server deja la pila
 * igual que Linux: argc, argv[], NULL, envp[], NULL, auxv[] (ver server/proc.c).
 */
    .text
    .global _start
    .type _start, @function
_start:
    xor %ebp, %ebp          /* marca el fin de la cadena de marcos */
    mov %rsp, %rdi          /* argumento: puntero a argc */
    and $-16, %rsp          /* alineación del ABI */
    call __posix_start
    hlt
    .section .note.GNU-stack,"",@progbits
