/*
 * login: asks for a user name and password, checks them against /etc/passwd and /etc/shadow
 * (crypt(3): SHA-512 "$6$..." and the other musl schemes), then becomes that user (groups from
 * /etc/group, gid, uid), goes to its home and starts its shell as a login shell.
 *   login              ask on the terminal (/sbin/init runs it on the console)
 *   login -f USER      no password (root only), as getty-like programs use it
 * An empty password field logs in without a password; "!" or "*" means locked.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#define _GNU_SOURCE
#include <crypt.h>
#include <grp.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int read_line(const char *prompt, char *buf, size_t len, int echo)
{
    struct termios old, quiet;
    int tty = isatty(0) && tcgetattr(0, &old) == 0;
    fputs(prompt, stdout);
    fflush(stdout);
    if (tty && !echo) {
        quiet = old;
        quiet.c_lflag &= ~ECHO;
        tcsetattr(0, TCSAFLUSH, &quiet);
    }
    /* one byte at a time: whatever follows the password belongs to the shell started next */
    size_t n = 0;
    char c;
    ssize_t got;
    while ((got = read(0, &c, 1)) == 1 && c != '\n')
        if (c != '\r' && n + 1 < len) buf[n++] = c;
    buf[n] = 0;
    if (tty && !echo) {
        tcsetattr(0, TCSAFLUSH, &old);
        fputs("\n", stdout);
    }
    return got == 1 || n ? 0 : -1;
}

/* The password hash: from /etc/shadow when /etc/passwd says "x" */
static const char *hash_of(const struct passwd *pw)
{
    if (strcmp(pw->pw_passwd, "x")) return pw->pw_passwd;
    struct spwd *sp = getspnam(pw->pw_name);
    return sp ? sp->sp_pwdp : "!";
}

static int password_ok(const struct passwd *pw, const char *given)
{
    const char *h = hash_of(pw);
    if (!*h) return 1;                                  /* no password */
    if (*h == '!' || *h == '*') return 0;               /* locked */
    const char *c = crypt(given, h);
    return c && !strcmp(c, h);
}

static void become(const struct passwd *pw)
{
    if (initgroups(pw->pw_name, pw->pw_gid) || setgid(pw->pw_gid) || setuid(pw->pw_uid)) {
        perror("login");
        exit(1);
    }
    if (chdir(pw->pw_dir)) chdir("/");
    const char *shell = pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
    clearenv();
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", shell, 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("PATH", pw->pw_uid ? "/bin:/usr/bin" : "/bin:/sbin:/usr/bin", 1);
    char arg0[64];
    const char *base = strrchr(shell, '/') ? strrchr(shell, '/') + 1 : shell;
    snprintf(arg0, sizeof arg0, "-%s", base);          /* "-zsh": a login shell */
    execl(shell, arg0, (char *)0);
    perror(shell);
    exit(127);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "-f")) {
        if (getuid() != 0) { fputs("login: -f: only root\n", stderr); return 1; }
        struct passwd *pw = getpwnam(argv[2]);
        if (!pw) { fprintf(stderr, "login: %s: no such user\n", argv[2]); return 1; }
        become(pw);
    }
    char host[64] = "gnu-sel4", name[64], pass[256], prompt[96];
    gethostname(host, sizeof host);
    snprintf(prompt, sizeof prompt, "%s login: ", host);
    for (int tries = 0; tries < 3; tries++) {
        if (read_line(prompt, name, sizeof name, 1)) return 1;
        if (!*name) { tries--; continue; }
        struct passwd *pw = getpwnam(name);
        const char *h = pw ? hash_of(pw) : "!";
        pass[0] = 0;
        if (*h && read_line("Password: ", pass, sizeof pass, 0)) return 1;
        if (pw && password_ok(pw, pass)) {
            memset(pass, 0, sizeof pass);
            become(pw);
        }
        memset(pass, 0, sizeof pass);
        sleep(2);                                       /* slow down guessing */
        puts("Login incorrect");
    }
    return 1;
}
