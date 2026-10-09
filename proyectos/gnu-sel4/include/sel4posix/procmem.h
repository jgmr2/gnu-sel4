/*
 * Mapa de memoria de un proceso de GNU/seL4.
 *
 * Cada proceso corre en una "ranura": un hilo con su propio espacio de
 * direcciones que posix_server crea la primera vez que hace falta (con
 * memoria de su untyped, ver server/mem.c) y reutiliza después. Las regiones
 * están en direcciones fijas (así fork() copia la memoria tal cual de una
 * ranura a otra), pero sus tamaños son solo límites virtuales: las páginas se
 * crean cuando el proceso las toca (fallo de página) y vuelven al servidor con
 * exit, exec y munmap. posix_server ve la memoria de cada ranura en una
 * ventana propia de 4 GiB (PROC_WIN_*), para cargar programas y copiar procesos.
 *
 *   0x1000_0000  imagen del programa (ELF estático enlazado aquí), rwx
 *   0x3000_0000  búfer compartido con posix_server (datos de las syscalls)
 *   0x3001_0000  búfer IPC de seL4
 *   0x4000_0000  heap (brk)
 *   0x6000_0000  pila (crece hacia abajo desde PROC_STACK_BASE + PROC_STACK_SIZE)
 *   0x8000_0000  mmap anónimo
 */
#pragma once

#define PROC_SLOTS        32                /* procesos vivos a la vez (= NPROC) */

#define PROC_IMG_BASE     0x10000000UL
#define PROC_IMG_SIZE     0x10000000UL      /* 256 MiB */
#define PROC_BUF_BASE     0x30000000UL      /* = POSIX_BUF_SIZE bytes */
#define PROC_IPCBUF       0x30010000UL      /* 4 KiB */
#define PROC_HEAP_BASE    0x40000000UL
#define PROC_HEAP_SIZE    0x10000000UL      /* 256 MiB */
#define PROC_STACK_BASE   0x60000000UL
#define PROC_STACK_SIZE   0x00800000UL      /* 8 MiB */
#define PROC_MMAP_BASE    0x80000000UL
#define PROC_MMAP_SIZE    0x40000000UL      /* 1 GiB */

/* Ventana de cada ranura dentro de posix_server: la dirección v del proceso
 * de la ranura s se ve en PROC_WIN_BASE + (s << 32) + v */
#define PROC_WIN_BASE     0x10000000000UL

/* Entradas propias en el vector auxiliar */
#define AT_SEL4_IPCBUF    0x5e14    /* dirección del búfer IPC de seL4 del proceso */
#define AT_SEL4_EPOCH     0x5e15    /* hora UNIX (s) leída del reloj CMOS al arrancar... */
#define AT_SEL4_TSC0      0x5e16    /* ...y el valor del TSC en ese momento */
#define AT_SEL4_TSCHZ     0x5e17    /* frecuencia del TSC (Hz) */

/* CSpace de un proceso: CNode de 128 entradas con la misma numeración que
 * libmicrokit (el runtime usa estas capacidades) */
#define PROC_CNODE_BITS   7
#define PROC_NTFN_CAP     1         /* notificación: el servidor despierta al proceso */
#define PROC_EP_CAP       74        /* endpoint del servidor (BASE_ENDPOINT_CAP + POSIX_CHANNEL) */

/* CSpace raíz de posix_server (64 entradas; ver <cspace> en meta.py). Un cptr
 * (n << 58) | i nombra la entrada i de la CNode instalada en la raíz n. */
#define SRV_ROOT_CNODE_SLOT  1      /* su CNode raíz (con guarda): root[i] = (1 << 58) | i */
#define SRV_POOL_SLOT        2      /* 2..3: untypeds del gestor de memoria */
#define SRV_NPOOLS           2
#define SRV_POOL_BYTES       (768UL << 20)  /* suma de SRV_POOL_SIZES en meta.py */
#define SRV_ASID_POOL_SLOT   5      /* pool de ASIDs para los espacios de direcciones nuevos */
#define SRV_TCB_SLOT         6      /* su propio TCB (autoridad de prioridad) */
#define SRV_EP_SLOT          7      /* su endpoint, para acuñar copias con insignia */
#define SRV_SCHED_POOL_SLOT  8      /* CNode con PROC_SLOTS scheduling contexts */
#define SRV_FREE_ROOT_SLOT   20     /* 20..63: libres para CNodes del gestor de memoria */
