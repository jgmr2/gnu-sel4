/*
 * Protocolo posix_server -> netstack (pila TCP/IP de FreeBSD).
 *
 * posix_server llama por PPC: MR0 = operación, MR1..MR4 = argumentos; los
 * datos van en net_buf (memoria compartida). Respuesta: MR0 = resultado
 * (>= 0) o -errno de Linux; MR1 = longitud de la dirección u opción devuelta.
 *
 * Sockets: el número que da NET_OP_SOCKET/NET_OP_ACCEPT. Todo usa la ABI de
 * Linux (familias, tipos, opciones, struct sockaddr): F-Stack la traduce.
 * Dentro de netstack los sockets nunca bloquean (-EAGAIN, -EINPROGRESS); la
 * espera la hace posix_server. netstack notifica a posix_server cuando hay
 * actividad en algún socket, para que reintente lo que estaba esperando.
 *
 * Diseño de net_buf (y del búfer compartido del proceso en estas llamadas):
 *   [0, NET_ADDR_OFF)          datos (send/recv) u opción (setsockopt/getsockopt)
 *   [NET_ADDR_OFF, +128)       dirección (bind, connect, accept, sendto, recvfrom...)
 */
#pragma once

#define NET_BUF_SIZE   0x10000
#define NET_ADDR_OFF   (NET_BUF_SIZE - 128)
#define NET_ADDR_MAX   128
#define NET_DATA_MAX   NET_ADDR_OFF

enum {
    NET_OP_SOCKET = 1,     /* dominio, tipo, protocolo                -> socket */
    NET_OP_BIND,           /* s, longitud de la dirección */
    NET_OP_CONNECT,        /* s, longitud de la dirección */
    NET_OP_LISTEN,         /* s, backlog */
    NET_OP_ACCEPT,         /* s                                        -> socket; dirección, MR1 */
    NET_OP_SEND,           /* s, len, flags, long. de dirección (0 = conectado) -> bytes */
    NET_OP_RECV,           /* s, len, flags                            -> bytes; dirección, MR1 */
    NET_OP_SHUTDOWN,       /* s, cómo */
    NET_OP_CLOSE,          /* s */
    NET_OP_SETSOCKOPT,     /* s, nivel, nombre, longitud */
    NET_OP_GETSOCKOPT,     /* s, nivel, nombre, longitud               -> opción, MR1 */
    NET_OP_GETSOCKNAME,    /* s                                        -> dirección, MR1 */
    NET_OP_GETPEERNAME,    /* s                                        -> dirección, MR1 */
    NET_OP_POLL,           /* s, eventos (POLLIN...)                   -> revents */
    NET_OP_FIONREAD,       /* s                                        -> bytes por leer */
};
