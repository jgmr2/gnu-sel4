/*
 * mount: the mounted file systems, and switching the root between read-only and read-write.
 * There is only one disk file system (fs_ext2, on /); after an unclean shutdown it starts
 * read-only, and /etc/rc goes read-write once e2fsck has checked /dev/root.
 *   mount                       lists them (/proc/mounts)
 *   mount -o remount,rw /       read-write (refused while the disk is not clean: run e2fsck)
 *   mount -o remount,ro /       read-only (written out and marked clean)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

int main(int argc, char **argv)
{
    if (argc == 1) {
        FILE *f = fopen("/proc/mounts", "r");
        if (!f) { perror("mount: /proc/mounts"); return 1; }
        char dev[64], dir[256], type[32], opts[64];
        while (fscanf(f, "%63s %255s %31s %63s %*d %*d", dev, dir, type, opts) == 4)
            printf("%s on %s type %s (%s)\n", dev, dir, type, opts);
        fclose(f);
        return 0;
    }
    if (argc != 4 || strcmp(argv[1], "-o") ||
        (strcmp(argv[2], "remount,rw") && strcmp(argv[2], "remount,ro"))) {
        fprintf(stderr, "usage: mount\n       mount -o remount,rw|remount,ro /\n");
        return 2;
    }
    unsigned long flags = MS_REMOUNT | (!strcmp(argv[2], "remount,ro") ? MS_RDONLY : 0);
    if (mount(NULL, argv[3], NULL, flags, NULL) == 0) return 0;
    if (errno == EUCLEAN)
        fprintf(stderr, "mount: %s: the file system is not clean: run e2fsck -f /dev/root first\n", argv[3]);
    else
        fprintf(stderr, "mount: %s: %s\n", argv[3], strerror(errno));
    return 32;
}
