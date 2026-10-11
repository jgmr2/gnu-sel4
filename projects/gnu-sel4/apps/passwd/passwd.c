/*
 * passwd: changes a password in /etc/shadow (installed set-user-ID root: users change their own).
 *   passwd            your password (asks the current one first)
 *   passwd USER       root: any user's, without the current one
 *   passwd -l USER    root: lock the account ("!" in front of the hash)
 *   chpasswd          root: "user:password" lines on standard input (scripts, tests)
 * New hashes are SHA-512 crypt ("$6$" + 16 random salt characters from getrandom).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/random.h>

#define SHADOW "/etc/shadow"

static const char *hash_new(const char *pass)
{
    static const char abc[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    unsigned char r[16];
    char salt[3 + 16 + 2] = "$6$";
    if (getrandom(r, sizeof r, 0) != sizeof r) return NULL;
    for (int i = 0; i < 16; i++) salt[3 + i] = abc[r[i] % 64];
    salt[19] = '$';
    salt[20] = 0;
    return crypt(pass, salt);
}

/* Rewrites /etc/shadow with the new hash for user (atomically: a new file, then rename) */
static int set_hash(const char *user, const char *hash, int lock)
{
    FILE *in = fopen(SHADOW, "r");
    if (!in) { perror(SHADOW); return 1; }
    int fd = open(SHADOW ".new", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!out) { perror(SHADOW ".new"); fclose(in); return 1; }
    char line[1024];
    int found = 0;
    size_t ul = strlen(user);
    while (fgets(line, sizeof line, in)) {
        if (!strncmp(line, user, ul) && line[ul] == ':') {
            char *rest = strchr(line + ul + 1, ':');          /* after the old hash */
            char old[512] = "";
            size_t hl = rest ? (size_t)(rest - (line + ul + 1)) : 0;
            if (hl < sizeof old) memcpy(old, line + ul + 1, hl), old[hl] = 0;
            if (lock) fprintf(out, "%s:%s%s%s", user, old[0] == '!' ? "" : "!", old, rest ? rest : ":\n");
            else fprintf(out, "%s:%s%s", user, hash, rest ? rest : ":\n");
            found = 1;
        } else {
            fputs(line, out);
        }
    }
    fclose(in);
    if (!found) fprintf(out, "%s:%s:20372:0:99999:7:::\n", user, lock ? "!" : hash);
    if (fflush(out) || fsync(fileno(out)) || fclose(out) || rename(SHADOW ".new", SHADOW)) {
        perror(SHADOW);
        unlink(SHADOW ".new");
        return 1;
    }
    return 0;
}

static int ask(const char *prompt, char *buf, size_t len)
{
    struct termios old, quiet;
    int tty = isatty(0) && tcgetattr(0, &old) == 0;
    fputs(prompt, stderr);
    if (tty) { quiet = old; quiet.c_lflag &= ~ECHO; tcsetattr(0, TCSAFLUSH, &quiet); }
    char *r = fgets(buf, len, stdin);
    if (tty) { tcsetattr(0, TCSAFLUSH, &old); fputs("\n", stderr); }
    if (!r) return -1;
    buf[strcspn(buf, "\r\n")] = 0;
    return 0;
}

static int chpasswd(void)
{
    if (getuid() != 0) { fputs("chpasswd: only root\n", stderr); return 1; }
    char line[512];
    int bad = 0;
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        if (!getpwnam(line)) { fprintf(stderr, "chpasswd: %s: no such user\n", line); bad = 1; continue; }
        const char *h = hash_new(colon + 1);
        if (!h || set_hash(line, h, 0)) bad = 1;
    }
    return bad;
}

int main(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (!strcmp(name, "chpasswd")) return chpasswd();
    int lock = argc > 1 && !strcmp(argv[1], "-l");
    const char *target = argc > 1 + lock ? argv[1 + lock] : NULL;
    struct passwd *me = getpwuid(getuid());
    if (!me) { fputs("passwd: who are you?\n", stderr); return 1; }
    if (!target) target = me->pw_name;
    if (getuid() != 0 && (lock || strcmp(target, me->pw_name))) {
        fputs("passwd: only root can change another user's password or lock an account\n", stderr);
        return 1;
    }
    if (!getpwnam(target)) { fprintf(stderr, "passwd: %s: no such user\n", target); return 1; }
    if (lock) return set_hash(target, NULL, 1);
    char cur[256], a[256], b[256];
    if (getuid() != 0) {                                /* your own: prove it first */
        struct spwd *sp = getspnam(target);
        const char *h = sp ? sp->sp_pwdp : "";
        if (*h) {
            if (ask("Current password: ", cur, sizeof cur)) return 1;
            const char *c = crypt(cur, h);
            memset(cur, 0, sizeof cur);
            if (!c || strcmp(c, h)) { sleep(2); fputs("passwd: wrong password\n", stderr); return 1; }
        }
    }
    if (ask("New password: ", a, sizeof a) || ask("Retype new password: ", b, sizeof b)) return 1;
    if (strcmp(a, b)) { fputs("passwd: the passwords do not match\n", stderr); return 1; }
    if (!*a) { fputs("passwd: empty password: nothing changed\n", stderr); return 1; }
    const char *h = hash_new(a);
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    if (!h) { fputs("passwd: no random salt\n", stderr); return 1; }
    if (set_hash(target, h, 0)) return 1;
    fprintf(stderr, "passwd: password for %s updated\n", target);
    return 0;
}
