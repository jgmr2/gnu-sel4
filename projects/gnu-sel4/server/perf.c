/*
 * Performance counters for /proc/perf: how many times something happened and how many TSC
 * cycles it took, since boot. Lines "name count cycles"; the first one gives the TSC frequency.
 * `perfstat command` prints the difference over one command.
 *
 *   sys.<name>      each system call, from the request to the reply (includes everything the
 *                   server did for it: fs_ext2 calls, copies, page creation)
 *   mem.fault       page faults served (a new page each); mem.fork_copy: pages copied by fork
 *   mem.free_all    returning a whole address space (exit and exec)
 *   fs.<op>         calls to fs_ext2 seen from posix_server (the protected call included)
 *   ext2.*          fs_ext2's own counters (FS_OP_STATS): operations, cache, disk requests
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <os/sddf.h>
#include <string.h>
#include <sddf/util/printf.h>
#include "server.h"
#include "syscall_names.h"

struct perf_ctr perf_sys[PERF_NSYS], perf_fs[32], perf_fault, perf_fork_copy, perf_free_all;

size_t perf_text(char *buf, size_t max)
{
    size_t n = 0;
#define OUT(...) (n += sddf_snprintf(buf + n, n < max ? max - n : 0, __VA_ARGS__))
    OUT("tsc_hz %" PRIu64 " 0\n", clock_tsc_hz);
    for (int i = 0; i < PERF_NSYS; i++) {
        if (!perf_sys[i].n) continue;
        if (i < (int)(sizeof(syscall_names) / sizeof(*syscall_names)) && syscall_names[i])
            OUT("sys.%s %" PRIu64 " %" PRIu64 "\n", syscall_names[i], perf_sys[i].n, perf_sys[i].cyc);
        else
            OUT("sys.%d %" PRIu64 " %" PRIu64 "\n", i, perf_sys[i].n, perf_sys[i].cyc);
    }
    OUT("mem.fault %" PRIu64 " %" PRIu64 "\n", perf_fault.n, perf_fault.cyc);
    OUT("mem.fork_copy %" PRIu64 " %" PRIu64 "\n", perf_fork_copy.n, perf_fork_copy.cyc);
    OUT("mem.free_all %" PRIu64 " %" PRIu64 "\n", perf_free_all.n, perf_free_all.cyc);
    static const char *fs_names[32] = {
        "0", "lookup", "stat", "read", "write", "truncate", "getdents", "readlink", "create",
        "unlink", "rename", "symlink", "chmod", "statfs", "setclock", "settimes", "chown", "link",
        "sync", "setcred", "stats",
    };
    for (int i = 0; i < 32; i++)
        if (perf_fs[i].n)
            OUT("fs.%s %" PRIu64 " %" PRIu64 "\n", fs_names[i] ? fs_names[i] : "other", perf_fs[i].n, perf_fs[i].cyc);
    if (n < max) n += vfs_fs_stats(buf + n, max - n);
    return n < max ? n : max;
#undef OUT
}
