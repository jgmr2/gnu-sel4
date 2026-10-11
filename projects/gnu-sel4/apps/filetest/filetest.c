/* File system test using plain POSIX functions. */
/* SPDX-License-Identifier: BSD-2-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <poll.h>

static int fails;
#define CHECK(cond, what) do { \
    if (cond) printf("  ok   %s\n", what); \
    else { printf("  FAIL %s (errno %d: %s)\n", what, errno, strerror(errno)); fails++; } \
} while (0)

static void ls(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { printf("  opendir(%s): %s\n", dir, strerror(errno)); return; }
    printf("  %s:\n", dir);
    struct dirent *e;
    while ((e = readdir(d))) {
        char path[512];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (lstat(path, &st)) continue;
        printf("    %c%03o %5lld  %s", S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : '-',
               st.st_mode & 0777, (long long)st.st_size, e->d_name);
        if (S_ISLNK(st.st_mode)) {
            char t[256];
            ssize_t n = readlink(path, t, sizeof t - 1);
            if (n >= 0) { t[n] = 0; printf(" -> %s", t); }
        }
        printf("\n");
    }
    closedir(d);
}

int main(void)
{
    char buf[512];

    printf("== read ==\n");
    FILE *f = fopen("/etc/motd", "r");
    CHECK(f != NULL, "fopen /etc/motd");
    if (f) { while (fgets(buf, sizeof buf, f)) printf("  | %s", buf); fclose(f); }
    ls("/");
    ls("/home/user");

    printf("== write ==\n");
    CHECK(mkdir("/tmp/test", 0755) == 0, "mkdir /tmp/test");
    CHECK(chdir("/tmp/test") == 0, "chdir /tmp/test");
    CHECK(getcwd(buf, sizeof buf) && !strcmp(buf, "/tmp/test"), "getcwd == /tmp/test");
    f = fopen("notes.txt", "w");
    CHECK(f != NULL, "fopen notes.txt w (relative)");
    for (int i = 0; i < 2000; i++) fprintf(f, "line %d of the big test file\n", i);   /* ~60 KiB: several blocks */
    fclose(f);
    struct stat st;
    CHECK(stat("notes.txt", &st) == 0 && st.st_size > 50000, "stat: size > 50 KiB");
    printf("       size %lld bytes, %lld 512-byte blocks\n", (long long)st.st_size, (long long)st.st_blocks);

    f = fopen("notes.txt", "r");
    int lines = 0, ok = 1;
    while (f && fgets(buf, sizeof buf, f)) {
        char exp[64];
        snprintf(exp, sizeof exp, "line %d of the big test file\n", lines);
        if (strcmp(buf, exp)) ok = 0;
        lines++;
    }
    if (f) fclose(f);
    CHECK(lines == 2000 && ok, "read the 2000 lines back unchanged");

    f = fopen("notes.txt", "a");
    fputs("line added at end\n", f);
    fclose(f);
    int fd = open("notes.txt", O_RDONLY);
    off_t end = lseek(fd, -18, SEEK_END);
    ssize_t n = read(fd, buf, 18);
    buf[n > 0 ? n : 0] = 0;
    close(fd);
    CHECK(end > 0 && !strcmp(buf, "line added at end\n"), "append + lseek(SEEK_END) + read");

    CHECK(truncate("notes.txt", 100) == 0 && stat("notes.txt", &st) == 0 && st.st_size == 100, "truncate to 100 bytes");
    CHECK(rename("notes.txt", "/home/user/notes.txt") == 0, "rename across directories");
    CHECK(access("/home/user/notes.txt", F_OK) == 0 && access("notes.txt", F_OK) != 0, "the file moved");
    CHECK(symlink("/home/user/notes.txt", "link") == 0, "symlink");
    f = fopen("link", "r");
    CHECK(f && fgets(buf, sizeof buf, f) && !strncmp(buf, "line 0 ", 7), "read through the link");
    if (f) fclose(f);
    CHECK(mkdir("sub", 0700) == 0 && rename("sub", "sub2") == 0, "mkdir + rename of a directory");
    CHECK(rmdir("/tmp/test") != 0 && errno == ENOTEMPTY, "rmdir of a non-empty dir -> ENOTEMPTY");
    CHECK(unlink("link") == 0 && rmdir("sub2") == 0, "unlink + rmdir");
    CHECK(chdir("/") == 0 && rmdir("/tmp/test") == 0, "rmdir /tmp/test");
    CHECK(open("/does/not/exist", O_RDONLY) < 0 && errno == ENOENT, "open of a missing file -> ENOENT");

    printf("== big file (doubly indirect blocks, larger than the cache) ==\n");
    {
        static unsigned char chunk[65536];
        int big = open("/big.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        int okw = big >= 0;
        for (int i = 0; okw && i < 96; i++) {                 /* 96 x 64 KiB = 6 MiB */
            for (int j = 0; j < (int)sizeof chunk; j++) chunk[j] = (unsigned char)(i * 7 + j);
            okw = write(big, chunk, sizeof chunk) == (ssize_t)sizeof chunk;
        }
        if (big >= 0) close(big);
        CHECK(okw && stat("/big.bin", &st) == 0 && st.st_size == 96 * 65536, "write 6 MiB");
        big = open("/big.bin", O_RDONLY);
        int okr = big >= 0;
        for (int i = 95; okr && i >= 0; i -= 5) {             /* scattered reads with pread */
            okr = pread(big, chunk, sizeof chunk, (off_t)i * 65536) == (ssize_t)sizeof chunk;
            for (int j = 0; okr && j < (int)sizeof chunk; j++) okr = chunk[j] == (unsigned char)(i * 7 + j);
        }
        if (big >= 0) close(big);
        CHECK(okr, "read back with pread and check every byte");
        struct statvfs a, b;
        statvfs("/", &a);
        CHECK(unlink("/big.bin") == 0, "delete the 6 MiB file");
        statvfs("/", &b);
        CHECK(b.f_bfree - a.f_bfree >= 1536, "its blocks were freed (data + indirect)");
        printf("       blocks freed: %lu\n", (unsigned long)(b.f_bfree - a.f_bfree));
    }

    printf("== named pipes (mkfifo) ==\n");
    {
        unlink("/tmp/fifo");
        CHECK(mkfifo("/tmp/fifo", 0600) == 0, "mkfifo");
        CHECK(lstat("/tmp/fifo", &st) == 0 && S_ISFIFO(st.st_mode), "stat sees S_IFIFO");
        DIR *d = opendir("/tmp");
        int dt = 0;
        struct dirent *e;
        while (d && (e = readdir(d))) if (!strcmp(e->d_name, "fifo")) dt = e->d_type;
        if (d) closedir(d);
        CHECK(dt == DT_FIFO, "readdir: d_type DT_FIFO");
        CHECK(open("/tmp/fifo", O_WRONLY | O_NONBLOCK) < 0 && errno == ENXIO, "write end without a reader -> ENXIO");
        int r = open("/tmp/fifo", O_RDONLY | O_NONBLOCK);
        CHECK(r >= 0, "read end with O_NONBLOCK opens at once");
        int w = open("/tmp/fifo", O_WRONLY | O_NONBLOCK);
        CHECK(w >= 0 && write(w, "abc", 3) == 3, "write end once there is a reader");
        char b[16] = { 0 };
        CHECK(read(r, b, sizeof b) == 3 && !memcmp(b, "abc", 3), "data from one end to the other");
        close(w);
        CHECK(read(r, b, sizeof b) == 0, "EOF when the last writer closes");
        close(r);
        pid_t pid = fork();                                 /* blocking opens meet each other */
        if (pid == 0) {
            int cw = open("/tmp/fifo", O_WRONLY);
            _exit(cw >= 0 && write(cw, "hello", 5) == 5 ? 0 : 1);
        }
        r = open("/tmp/fifo", O_RDONLY);
        memset(b, 0, sizeof b);
        ssize_t n = r >= 0 ? read(r, b, sizeof b) : -1;
        int status;
        waitpid(pid, &status, 0);
        CHECK(n == 5 && !memcmp(b, "hello", 5) && WIFEXITED(status) && !WEXITSTATUS(status),
              "blocking open(O_RDONLY) waits for the writer");
        if (r >= 0) close(r);
        int rw = open("/tmp/fifo", O_RDWR | O_NONBLOCK);
        struct pollfd pf = { .fd = rw, .events = POLLIN | POLLOUT };
        CHECK(rw >= 0 && poll(&pf, 1, 0) == 1 && (pf.revents & POLLOUT) && write(rw, "x", 1) == 1 &&
              poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN) && read(rw, b, 1) == 1, "O_RDWR reads and writes (s6's trick)");
        if (rw >= 0) close(rw);
        CHECK(unlink("/tmp/fifo") == 0, "unlink the FIFO");
    }

    struct statvfs sv;
    CHECK(statvfs("/", &sv) == 0, "statvfs /");
    printf("       %lu MiB in total, %lu MiB free\n",
           (unsigned long)(sv.f_blocks * sv.f_frsize >> 20), (unsigned long)(sv.f_bfree * sv.f_frsize >> 20));
    ls("/home/user");

    printf("== %s ==\n", fails ? "THERE ARE FAILURES" : "all good");
    return fails;
}
