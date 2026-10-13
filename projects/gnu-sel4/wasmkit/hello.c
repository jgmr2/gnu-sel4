/*
 * wasmkit's first program with a libc (TODO item 24), as /sbin/init: plain C on musl and the
 * runtime, compiled with sel4-cc for wasm32. stdio on the console, a file through posix_server
 * and fs_ext2, malloc, the clock, sleep, and its arguments; then it becomes coreutils' ls.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

int main(int argc, char **argv)
{
    struct utsname u;
    uname(&u);
    printf("\nhello from C on musl, a WebAssembly process: %s %s (%s), pid %d, argv[0] %s\n",
           u.sysname, u.release, u.machine, getpid(), argc ? argv[0] : "?");
    FILE *f = fopen("/etc/os-release", "r");
    char line[256];
    while (f && fgets(line, sizeof line, f)) fputs(line, stdout);
    if (f) fclose(f);
    char *big = malloc(8 << 20);
    memset(big, 'x', 8 << 20);
    printf("malloc'd and wrote 8 MiB: %s\n", big[(8 << 20) - 1] == 'x' ? "ok" : "WRONG");
    time_t t = time(NULL);
    printf("the time: %s", ctime(&t));
    printf("pi = %.6f, 2^64 = %.0Lf\n", 3.14159265358979, 18446744073709551616.0L);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    sleep(1);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("slept %ld ms. Now exec: ls -l /bin/ls /bin/coreutils (GNU coreutils for wasm32)\n",
           (long)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000));
    fflush(stdout);
    if (access("/bin/coreutils", X_OK) == 0) {     /* no fork yet: ls takes this process's place */
        execl("/bin/ls", "ls", "-l", "/bin/ls", "/bin/coreutils", (char *)0);
        perror("execl");
    }
    return 0;
}
