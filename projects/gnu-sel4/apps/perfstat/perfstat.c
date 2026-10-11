/*
 * perfstat: runs a command and prints what changed in /proc/perf while it ran (like perf stat).
 *   perfstat COMMAND [ARGS...]     the counters that changed, the most expensive first
 *   perfstat                       everything since boot
 * Columns: how many times, total time, time per call. Times come from TSC cycles. The two
 * reads of /proc/perf themselves add a few sys.open/read/close calls to the result.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#define MAXC 640

struct ctr { char name[48]; unsigned long long n, cyc; };

static int load(struct ctr *c, unsigned long long *hz)
{
    FILE *f = fopen("/proc/perf", "r");
    if (!f) { perror("/proc/perf"); exit(1); }
    int k = 0;
    while (k < MAXC && fscanf(f, "%47s %llu %llu", c[k].name, &c[k].n, &c[k].cyc) == 3) {
        if (!strcmp(c[k].name, "tsc_hz")) { *hz = c[k].n; continue; }
        k++;
    }
    fclose(f);
    return k;
}

static int by_time(const void *a, const void *b)
{
    const struct ctr *x = a, *y = b;
    if (x->cyc != y->cyc) return x->cyc < y->cyc ? 1 : -1;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static void show(struct ctr *c, int k, unsigned long long hz)
{
    qsort(c, k, sizeof *c, by_time);
    printf("%-24s %10s %12s %12s\n", "counter", "count", "total ms", "per call us");
    for (int i = 0; i < k; i++) {
        if (!c[i].n) continue;
        double ms = hz ? c[i].cyc * 1e3 / hz : 0, us = c[i].n ? ms * 1e3 / c[i].n : 0;
        if (c[i].cyc) printf("%-24s %10llu %12.3f %12.3f\n", c[i].name, c[i].n, ms, us);
        else          printf("%-24s %10llu %12s %12s\n", c[i].name, c[i].n, "-", "-");
    }
}

int main(int argc, char **argv)
{
    static struct ctr a[MAXC], b[MAXC];
    unsigned long long hz = 0;
    int ka = load(a, &hz);
    if (argc < 2) {
        show(a, ka, hz);
        return 0;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        execvp(argv[1], argv + 1);
        perror(argv[1]);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int kb = load(b, &hz);
    for (int i = 0; i < kb; i++) {                 /* b - a, by name */
        for (int j = 0; j < ka; j++)
            if (!strcmp(a[j].name, b[i].name)) { b[i].n -= a[j].n; b[i].cyc -= a[j].cyc; break; }
        if (!strcmp(b[i].name, "ext2.cache_slots")) b[i].n = 0;
    }
    fflush(stdout);
    fprintf(stderr, "\n%s: %.3f s", argv[1], (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9);
    if (WIFEXITED(st) && WEXITSTATUS(st)) fprintf(stderr, " (exit %d)", WEXITSTATUS(st));
    if (WIFSIGNALED(st)) fprintf(stderr, " (signal %d)", WTERMSIG(st));
    fprintf(stderr, "\n");
    show(b, kb, hz);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
