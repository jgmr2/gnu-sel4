/*
 * Protocolo entre el VFS (posix_server) y un servidor de sistema de archivos
 * (fs_ext2, y en el futuro fs_ufs, ramfs...).
 *
 * Cada operación es una llamada protegida (microkit_ppcall) síncrona:
 *   MR0 = operación (FS_OP_*), MR1..MR4 = argumentos
 * Respuesta:
 *   MR0 = resultado (>= 0) o -errno; MR1.. = resultados extra según la operación
 *
 * Rutas y datos viajan por un búfer compartido de FS_BUF_SIZE bytes. Las
 * rutas son absolutas y ya normalizadas por el VFS (sin "." ni "..").
 * Cuando una operación lleva dos rutas, la segunda empieza en FS_BUF_PATH2.
 */
#pragma once

#define FS_BUF_SIZE   0x10000      /* 64 KiB, igual que POSIX_BUF_SIZE */
#define FS_BUF_PATH2  0x8000
#define FS_PATH_MAX   4096

enum fs_op {
    /* buf: ruta. MR1: 1 = seguir el último enlace simbólico.
     * -> inodo; MR1 = modo (st_mode), MR2 = tamaño */
    FS_OP_LOOKUP = 1,
    /* MR1: inodo -> buf: struct stat de Linux x86_64 (144 bytes) */
    FS_OP_STAT,
    /* MR1: inodo, MR2: desplazamiento, MR3: bytes -> buf: datos; resultado: bytes leídos */
    FS_OP_READ,
    /* MR1: inodo, MR2: desplazamiento, MR3: bytes; buf: datos -> bytes escritos */
    FS_OP_WRITE,
    /* MR1: inodo, MR2: tamaño nuevo */
    FS_OP_TRUNCATE,
    /* MR1: inodo de directorio, MR2: posición, MR3: bytes máx.
     * -> buf: struct linux_dirent64[]; resultado: bytes; MR1 = posición siguiente */
    FS_OP_GETDENTS,
    /* MR1: inodo -> buf: destino del enlace (sin '\0'); resultado: longitud */
    FS_OP_READLINK,
    /* buf: ruta, MR1: modo (S_IFREG o S_IFDIR | permisos) -> inodo creado */
    FS_OP_CREATE,
    /* buf: ruta, MR1: 1 = es rmdir */
    FS_OP_UNLINK,
    /* buf: ruta vieja, buf+FS_BUF_PATH2: ruta nueva */
    FS_OP_RENAME,
    /* buf: destino del enlace, buf+FS_BUF_PATH2: ruta del enlace nuevo */
    FS_OP_SYMLINK,
    /* MR1: inodo, MR2: modo nuevo (solo permisos) */
    FS_OP_CHMOD,
    /* -> buf: struct statfs de Linux x86_64 (120 bytes) */
    FS_OP_STATFS,
    /* MR1: hora UNIX al arrancar, MR2: TSC en ese momento, MR3: frecuencia del TSC */
    FS_OP_SETCLOCK,
    /* MR1: inodo, MR2: atime, MR3: mtime (segundos UNIX; -1 = no cambiar) */
    FS_OP_SETTIMES,
    /* MR1: inodo, MR2: uid, MR3: gid (-1 = no cambiar) */
    FS_OP_CHOWN,
    /* buf: ruta existente, buf+FS_BUF_PATH2: nombre nuevo (enlace duro) */
    FS_OP_LINK,
};
