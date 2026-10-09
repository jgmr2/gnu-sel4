/* Prueba del sistema de archivos con funciones POSIX normales. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

static int fails;
#define CHECK(cond, what) do { \
    if (cond) printf("  ok   %s\n", what); \
    else { printf("  FALLA %s (errno %d: %s)\n", what, errno, strerror(errno)); fails++; } \
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

    printf("== leer ==\n");
    FILE *f = fopen("/etc/motd", "r");
    CHECK(f != NULL, "fopen /etc/motd");
    if (f) { while (fgets(buf, sizeof buf, f)) printf("  | %s", buf); fclose(f); }
    ls("/");
    ls("/home/usuario");

    printf("== escribir ==\n");
    CHECK(mkdir("/tmp/prueba", 0755) == 0, "mkdir /tmp/prueba");
    CHECK(chdir("/tmp/prueba") == 0, "chdir /tmp/prueba");
    CHECK(getcwd(buf, sizeof buf) && !strcmp(buf, "/tmp/prueba"), "getcwd == /tmp/prueba");
    f = fopen("notas.txt", "w");
    CHECK(f != NULL, "fopen notas.txt w (relativa)");
    for (int i = 0; i < 2000; i++) fprintf(f, "línea %d del archivo grande\n", i);   /* ~60 KiB: varios bloques */
    fclose(f);
    struct stat st;
    CHECK(stat("notas.txt", &st) == 0 && st.st_size > 50000, "stat: tamaño > 50 KiB");
    printf("       tamaño %lld bytes, %lld bloques de 512\n", (long long)st.st_size, (long long)st.st_blocks);

    f = fopen("notas.txt", "r");
    int lines = 0, ok = 1;
    while (f && fgets(buf, sizeof buf, f)) {
        char exp[64];
        snprintf(exp, sizeof exp, "línea %d del archivo grande\n", lines);
        if (strcmp(buf, exp)) ok = 0;
        lines++;
    }
    if (f) fclose(f);
    CHECK(lines == 2000 && ok, "releer las 2000 líneas idénticas");

    f = fopen("notas.txt", "a");
    fputs("agregada al final\n", f);
    fclose(f);
    int fd = open("notas.txt", O_RDONLY);
    off_t end = lseek(fd, -18, SEEK_END);
    ssize_t n = read(fd, buf, 18);
    buf[n > 0 ? n : 0] = 0;
    close(fd);
    CHECK(end > 0 && !strcmp(buf, "agregada al final\n"), "append + lseek(SEEK_END) + read");

    CHECK(truncate("notas.txt", 100) == 0 && stat("notas.txt", &st) == 0 && st.st_size == 100, "truncate a 100 bytes");
    CHECK(rename("notas.txt", "/home/usuario/notas.txt") == 0, "rename entre directorios");
    CHECK(access("/home/usuario/notas.txt", F_OK) == 0 && access("notas.txt", F_OK) != 0, "el archivo se movió");
    CHECK(symlink("/home/usuario/notas.txt", "enlace") == 0, "symlink");
    f = fopen("enlace", "r");
    CHECK(f && fgets(buf, sizeof buf, f) && !strncmp(buf, "línea 0", 7), "leer a través del enlace");
    if (f) fclose(f);
    CHECK(mkdir("sub", 0700) == 0 && rename("sub", "sub2") == 0, "mkdir + rename de directorio");
    CHECK(rmdir("/tmp/prueba") != 0 && errno == ENOTEMPTY, "rmdir de dir no vacío -> ENOTEMPTY");
    CHECK(unlink("enlace") == 0 && rmdir("sub2") == 0, "unlink + rmdir");
    CHECK(chdir("/") == 0 && rmdir("/tmp/prueba") == 0, "rmdir /tmp/prueba");
    CHECK(open("/no/existe", O_RDONLY) < 0 && errno == ENOENT, "open inexistente -> ENOENT");

    printf("== archivo grande (bloques doblemente indirectos, más que la caché) ==\n");
    {
        static unsigned char chunk[65536];
        int big = open("/grande.bin", O_CREAT | O_WRONLY | O_TRUNC, 0644);
        int okw = big >= 0;
        for (int i = 0; okw && i < 96; i++) {                 /* 96 x 64 KiB = 6 MiB */
            for (int j = 0; j < (int)sizeof chunk; j++) chunk[j] = (unsigned char)(i * 7 + j);
            okw = write(big, chunk, sizeof chunk) == (ssize_t)sizeof chunk;
        }
        if (big >= 0) close(big);
        CHECK(okw && stat("/grande.bin", &st) == 0 && st.st_size == 96 * 65536, "escribir 6 MiB");
        big = open("/grande.bin", O_RDONLY);
        int okr = big >= 0;
        for (int i = 95; okr && i >= 0; i -= 5) {             /* lectura salteada con pread */
            okr = pread(big, chunk, sizeof chunk, (off_t)i * 65536) == (ssize_t)sizeof chunk;
            for (int j = 0; okr && j < (int)sizeof chunk; j++) okr = chunk[j] == (unsigned char)(i * 7 + j);
        }
        if (big >= 0) close(big);
        CHECK(okr, "releer con pread y verificar cada byte");
        struct statvfs a, b;
        statvfs("/", &a);
        CHECK(unlink("/grande.bin") == 0, "borrar el archivo de 6 MiB");
        statvfs("/", &b);
        CHECK(b.f_bfree - a.f_bfree >= 1536, "se liberaron sus bloques (datos + indirectos)");
        printf("       bloques liberados: %lu\n", (unsigned long)(b.f_bfree - a.f_bfree));
    }

    struct statvfs sv;
    CHECK(statvfs("/", &sv) == 0, "statvfs /");
    printf("       %lu MiB en total, %lu MiB libres\n",
           (unsigned long)(sv.f_blocks * sv.f_frsize >> 20), (unsigned long)(sv.f_bfree * sv.f_frsize >> 20));
    ls("/home/usuario");

    printf("== %s ==\n", fails ? "HAY FALLAS" : "todo bien");
    return fails;
}
