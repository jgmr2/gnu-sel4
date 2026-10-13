/*
 * /sbin/init on the browser port (TODO item 24) while there is no fork: the console as the
 * terminal of process 1's session, then a login shell in its place (apps/init.c starts it as a
 * child and runs /etc/rc first). When the shell exits, posix_server starts init again.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    setsid();
    int fd = open("/dev/console", O_RDWR);
    if (fd >= 0) {
        ioctl(fd, TIOCSCTTY, 0);
        for (int i = 0; i < 3; i++) dup2(fd, i);
        if (fd > 2) close(fd);
    }
    printf("\nGNU/seL4 in WebAssembly: posix_server, fs_ext2 and dash, no emulator (no fork yet:\n"
           "builtins and exec only)\n\n");
    fflush(stdout);
    execl("/bin/sh", "-sh", (char *)0);
    perror("init: /bin/sh");
    return 1;
}
