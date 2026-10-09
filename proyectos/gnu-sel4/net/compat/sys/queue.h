/* Lo que usan las cabeceras de F-Stack de <sys/queue.h> de BSD (musl no la trae).
 * Definiciones de FreeBSD sys/sys/queue.h (BSD-3-Clause, The Regents of the University of California). */
#pragma once

#define SLIST_HEAD(name, type)                                          \
struct name {                                                           \
        struct type *slh_first; /* first element */                     \
}

#define TAILQ_HEAD(name, type)                                          \
struct name {                                                           \
        struct type *tqh_first; /* first element */                     \
        struct type **tqh_last; /* addr of last next element */         \
}
