/*
 * Protocolo entre el runtime POSIX (en cada programa) y posix_server.
 *
 * Una llamada al sistema viaja como una llamada protegida de seL4 (PPC,
 * microkit_ppcall): síncrona, como un syscall de Linux.
 *   MR0      = número de syscall (numeración de Linux x86_64, la que usa musl)
 *   MR1..MR6 = argumentos
 * Respuesta:
 *   MR0      = valor de retorno (negativo = -errno, como el kernel Linux)
 *
 * Los argumentos que son punteros no cruzan: el runtime copia los datos a un
 * búfer compartido (POSIX_BUF_SIZE bytes) y sustituye el puntero por el
 * desplazamiento dentro del búfer.
 *
 * Llamadas que bloquean (read sin datos, write con la consola llena): el
 * servidor no puede dormir a mitad de una PPC, así que responde POSIX_RETRY.
 * El runtime entonces espera una notificación de seL4 por el mismo canal
 * (el servidor la manda cuando ya hay datos o espacio) y reintenta.
 */
#pragma once

#define POSIX_CHANNEL   0           /* canal del programa hacia el servidor */
#define POSIX_BUF_SIZE  0x10000     /* 64 KiB */
#define POSIX_NUM_MRS   7
/* Llamadas con dos rutas (rename, symlink...): la segunda va en esta posición */
#define POSIX_BUF_PATH2 0x8000

/* En cada respuesta, MR6 lleva las señales pendientes del proceso (bit s-1 = señal s) */
#define POSIX_MR_SIGNALS 6

/* Fuera del rango de errno (-1..-4095): nunca llega al programa */
#define POSIX_RETRY     (-0x7000)
