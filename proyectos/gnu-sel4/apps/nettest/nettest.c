/*
 * nettest: pruebas de la red nativa (pila TCP/IP de FreeBSD en netstack).
 *   nettest [host]      loopback + DNS (UDP) + petición HTTP (TCP) a host (por omisión example.com)
 *   nettest local       solo lo que no necesita internet (loopback)
 *   nettest servir [p]  servidor TCP en el puerto p (8080): responde y termina
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

static int fallos;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FALLA", what);
    if (!ok) fallos++;
}

static int servir(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) {
        perror("bind/listen");
        return 1;
    }
    printf("escuchando en el puerto %d...\n", port);
    fflush(stdout);
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    int c = accept(s, (struct sockaddr *)&peer, &plen);
    if (c < 0) {
        perror("accept");
        return 1;
    }
    printf("conexión de %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
    char buf[512];
    ssize_t n = read(c, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        printf("recibido: %.40s...\n", buf);
    }
    static const char resp[] = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n"
                               "hola desde un proceso de GNU/seL4 (pila TCP/IP de FreeBSD)\n";
    write(c, resp, sizeof resp - 1);
    close(c);
    close(s);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "servir")) return servir(argc > 2 ? atoi(argv[2]) : 8080);
    const char *host = argc > 1 ? argv[1] : "example.com";

    printf("== nettest: %s ==\n", host);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    check(s >= 0, "socket(AF_INET, SOCK_STREAM)");
    close(s);

    /* Loopback: servidor en 127.0.0.1 en un hijo, cliente aquí */
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
        ok = ok && connect(c, (struct sockaddr *)&la, sizeof la) == 0 && write(c, "eco", 3) == 3 &&
             read(c, b, sizeof b) == 3 && !memcmp(b, "eco", 3);
        check(ok, "TCP por loopback (127.0.0.1): eco");
        close(c);
        if (pid > 0) waitpid(pid, NULL, 0);
    }
    if (argc > 1 && !strcmp(argv[1], "local")) {
        printf(fallos ? "== %d fallos ==\n" : "== todo bien ==\n", fallos);
        return fallos != 0;
    }

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *ai = NULL;
    int e = getaddrinfo(host, "80", &hints, &ai);
    check(e == 0 && ai, "DNS (getaddrinfo, por UDP)");
    if (e || !ai) {
        printf("        %s\n", gai_strerror(e));
        return 1;
    }
    char ip[64];
    inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, ip, sizeof ip);
    printf("        %s -> %s\n", host, ip);

    s = socket(AF_INET, SOCK_STREAM, 0);
    check(connect(s, ai->ai_addr, ai->ai_addrlen) == 0, "connect TCP al puerto 80");
    char req[256];
    int n = snprintf(req, sizeof req, "GET / HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    check(write(s, req, n) == n, "envío de la petición HTTP");

    struct pollfd p = { .fd = s, .events = POLLIN };
    check(poll(&p, 1, 5000) == 1 && (p.revents & POLLIN), "poll espera la respuesta");
    static char resp[65536];
    size_t total = 0;
    ssize_t r;
    while ((r = read(s, resp + total, sizeof resp - 1 - total)) > 0) total += r;
    resp[total] = 0;
    check(total > 12 && !strncmp(resp, "HTTP/1.", 7), "respuesta HTTP completa hasta EOF");
    char *eol = strchr(resp, '\r');
    if (eol) *eol = 0;
    printf("        %s (%zu bytes)\n", resp, total);
    close(s);
    freeaddrinfo(ai);

    printf(fallos ? "== %d fallos ==\n" : "== todo bien ==\n", fallos);
    return fallos != 0;
}
