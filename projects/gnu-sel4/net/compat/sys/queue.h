/* What the F-Stack headers use from BSD <sys/queue.h> (musl does not ship it).
 * Definitions from FreeBSD sys/sys/queue.h.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 1991, 1993 The Regents of the University of California. All rights reserved.
 * (full license text: /usr/share/licenses/freebsd/BSD-3-Clause, see THIRD_PARTY.md) */
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
