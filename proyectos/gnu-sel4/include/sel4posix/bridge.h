/*
 * Puente GNU/seL4 <-> VM Linux (como el "interop" de WSL).
 *
 * posix_server y la VM comparten un canal de bytes: para Linux es la consola
 * virtio /dev/hvc1, donde escucha sel4-agent (vm/agent/). Por el canal viajan
 * tramas de sesiones multiplexadas; cada sesión es un comando ejecutado en la
 * VM a pedido de un programa nativo (apps/linux: /bin/linux, /bin/docker...).
 *
 * Trama en el canal:  struct br_frame + `len` bytes de datos
 *
 * Del lado nativo, un programa abre /dev/linux (una sesión) y habla en
 * "registros": cada write() es un registro [tipo][datos...] y cada read()
 * devuelve uno. posix_server les agrega/quita el número de sesión.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>

struct br_frame {
    uint8_t  type;
    uint8_t  pad;
    uint16_t session;
    uint32_t len;
};

#define BR_MAX_DATA   32768        /* datos máximos por trama/registro */

enum br_type {
    /* agente -> servidor */
    BR_HELLO     = 1,              /* el agente arrancó (sesiones anteriores quedan muertas) */
    BR_STDOUT    = 16,             /* salida estándar del comando */
    BR_STDERR    = 17,             /* salida de error */
    BR_EXIT      = 18,             /* terminó: datos = int32 código (0..255; 128+señal) */

    /* programa nativo -> agente */
    BR_OPEN      = 2,              /* datos = struct br_open + argv (cadenas terminadas en \0) */
    BR_STDIN     = 3,              /* datos para la entrada estándar */
    BR_STDIN_EOF = 4,              /* fin de la entrada estándar */
    BR_SIGNAL    = 5,              /* datos = int32 señal para el grupo del comando */
    BR_WINSIZE   = 6,              /* datos = uint16 filas, uint16 columnas */
    BR_CLOSE     = 7,              /* (lo genera el servidor al cerrarse la sesión) */
};

#define BR_OPEN_TTY   1            /* ejecutar con terminal (pty): modo interactivo */

struct br_open {
    uint32_t flags;
    uint16_t rows, cols;
    uint32_t argc;
    /* siguen argc cadenas */
};
