/*
 * nettest: native networking tests (FreeBSD TCP/IP stack in netstack).
 *   nettest [host]      loopback + DNS (UDP) + HTTP request (TCP) to host (default example.com)
 *   nettest local       only what needs no internet (loopback)
 *   nettest serve [p]   TCP server on port p (8080): replies once and exits
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int serve(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) {
        perror("bind/listen");
        return 1;
    }
    printf("listening on port %d...\n", port);
    fflush(stdout);
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    int c = accept(s, (struct sockaddr *)&peer, &plen);
    if (c < 0) {
        perror("accept");
        return 1;
    }
    printf("connection from %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
    char buf[512];
    ssize_t n = read(c, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        printf("received: %.40s...\n", buf);
    }
    static const char resp[] = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n"
                               "hello from a GNU/seL4 process (FreeBSD TCP/IP stack)\n";
    write(c, resp, sizeof resp - 1);
    close(c);
    close(s);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "serve")) return serve(argc > 2 ? atoi(argv[2]) : 8080);
    const char *host = argc > 1 ? argv[1] : "example.com";

    printf("== nettest: %s ==\n", host);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    check(s >= 0, "socket(AF_INET, SOCK_STREAM)");
    close(s);

    /* Loopback: server on 127.0.0.1 in a child, client here */
    {
        int ls = socket(AF_INET, SOCK_STREAM, 0), on = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        struct sockaddr_in la = { .sin_family = AF_INET, .sin_port = htons(9099),
                                  .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
        int ok = bind(ls, (struct sockaddr *)&la, sizeof la) == 0 && listen(ls, 2) == 0;
        pid_t pid = ok ? fork() : -1;
        if (pid == 0) {
            int c = accept(ls, NULL, NULL);
            char b[16];
            ssize_t n = read(c, b, sizeof b);
            if (n > 0) write(c, b, n);
            _exit(0);
        }
        close(ls);
        int c = socket(AF_INET, SOCK_STREAM, 0);
        char b[16] = { 0 };
        ok = ok && connect(c, (struct sockaddr *)&la, sizeof la) == 0 && write(c, "echo", 4) == 4 &&
             read(c, b, sizeof b) == 4 && !memcmp(b, "echo", 4);
        check(ok, "TCP over loopback (127.0.0.1): echo");
        close(c);
        if (pid > 0) waitpid(pid, NULL, 0);
    }
    if (argc > 1 && !strcmp(argv[1], "local")) {
        printf(failures ? "== %d failures ==\n" : "== all good ==\n", failures);
        return failures != 0;
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    int e = getaddrinfo(host, "80", &hints, &ai);
    check(e == 0 && ai, "DNS (getaddrinfo, over UDP)");
    if (e || !ai) {
        printf("        %s\n", gai_strerror(e));
        return 1;
    }
    char ip[64];
    inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, sizeof ip);
    printf("        %s -> %s\n", host, ip);

    s = socket(AF_INET, SOCK_STREAM, 0);
    check(connect(s, ai->ai_addr, ai->ai_addrlen) == 0, "TCP connect to port 80");
    char req[256];
    int n = snprintf(req, sizeof req, "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    check(write(s, req, n) == n, "sending the HTTP request");

    struct pollfd p = { .fd = s, .events = POLLIN };
    check(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN), "poll waits for the reply");
    static char resp[65536];
    size_t total = 0;
    ssize_t r;
    while ((r = read(s, resp + total, sizeof resp - 1 - total)) > 0) total += r;
    resp[total] = 0;
    check(total > 12 && !strncmp(resp, "HTTP/1.", 7), "full HTTP reply up to EOF");
    char *eol = strchr(resp, '\r');
    if (eol) *eol = 0;
    printf("        %s (%zu bytes)\n", resp, total);
    close(s);
    freeaddrinfo(ai);

    printf(failures ? "== %d failures ==\n" : "== all good ==\n", failures);
    return failures != 0;
}
