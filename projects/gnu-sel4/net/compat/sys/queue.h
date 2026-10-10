/* What the F-Stack headers use from BSD <sys/queue.h> (musl does not ship it).
 * Definitions from FreeBSD sys/sys/queue.h (BSD-3-Clause, The Regents of the University of California). */
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
