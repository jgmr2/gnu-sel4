/*
 * red: ver y cambiar la configuración de red de GNU/seL4 (netstack).
 *   red                                    estado (como cat /proc/net/dhcp)
 *   red dhcp                               dirección por DHCP
 *   red fija IP/PREFIJO [puerta IP] [dns IP [IP]]   dirección fija
 *   red aplicar                            lo que diga /etc/red.conf (lo hace /etc/rc al arrancar)
 * /etc/red.conf: líneas "modo dhcp|fija", "direccion IP/PREFIJO", "puerta IP", "dns IP [IP]".
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sel4posix/net_protocol.h>

static int uso(void)
{
    fprintf(stderr, "uso: red | red dhcp | red fija IP/PREFIJO [puerta IP] [dns IP [IP]] | red aplicar\n");
    return 2;
}

static int ip_prefijo(const char *s, uint32_t *ip, uint32_t *mask)
{
    char b[32];
    snprintf(b, sizeof b, "%s", s);
    char *barra = strchr(b, '/');
    int pre = 24;
    if (barra) {
        *barra = 0;
        pre = atoi(barra + 1);
        if (pre < 1 || pre > 32) return -1;
    }
    *mask = htonl(pre == 32 ? 0xffffffffu : ~(0xffffffffu >> pre));
    return inet_pton(AF_INET, b, ip) == 1 ? 0 : -1;
}

static int aplicar(struct net_cfg *c)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0 || ioctl(s, NET_SIOC_CONFIG, c) < 0) {
        perror("red");
        return 1;
    }
    close(s);
    return 0;
}

static int mostrar(void)
{
    FILE *f = fopen("/proc/net/dhcp", "r");
    if (!f) { perror("/proc/net/dhcp"); return 1; }
    char l[160];
    while (fgets(l, sizeof l, f)) fputs(l, stdout);
    fclose(f);
    return 0;
}

/* Interpreta "fija IP/P puerta G dns D [D]" (argv) */
static int fija(int argc, char **argv, struct net_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->mode = NET_CFG_FIJA;
    if (argc < 1 || ip_prefijo(argv[0], &c->ip, &c->mask)) return -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "puerta") && i + 1 < argc) {
            if (inet_pton(AF_INET, argv[++i], &c->gw) != 1) return -1;
        } else if (!strcmp(argv[i], "dns") && i + 1 < argc) {
            if (inet_pton(AF_INET, argv[++i], &c->dns[0]) != 1) return -1;
            if (i + 1 < argc && inet_pton(AF_INET, argv[i + 1], &c->dns[1]) == 1) i++;
        } else {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct net_cfg c = { .mode = NET_CFG_DHCP };
    if (argc == 1) return mostrar();
    if (!strcmp(argv[1], "dhcp")) return aplicar(&c);
    if (!strcmp(argv[1], "fija")) return fija(argc - 2, argv + 2, &c) ? uso() : aplicar(&c);
    if (!strcmp(argv[1], "aplicar")) {
        FILE *f = fopen("/etc/red.conf", "r");
        if (!f) return 0;                              /* sin archivo: DHCP (lo que ya hace netstack) */
        char l[160], *args[8];
        int n = 0, modo_fija = 0;
        static char guardado[8][40];
        while (fgets(l, sizeof l, f)) {
            char *k = strtok(l, " \t\r\n"), *v;
            if (!k || *k == '#') continue;
            if (!strcmp(k, "modo") && (v = strtok(NULL, " \t\r\n"))) modo_fija = !strcmp(v, "fija");
            else if (!strcmp(k, "direccion") && (v = strtok(NULL, " \t\r\n")) && n < 1) {
                snprintf(guardado[n], 40, "%s", v); args[n] = guardado[n]; n++;
            } else if ((!strcmp(k, "puerta") || !strcmp(k, "dns")) && n > 0) {
                for (char *w = k; w && n < 8; w = strtok(NULL, " \t\r\n")) {
                    snprintf(guardado[n], 40, "%s", w); args[n] = guardado[n]; n++;
                }
            }
        }
        fclose(f);
        if (!modo_fija) return aplicar(&c);
        if (fija(n, args, &c)) { fprintf(stderr, "red: /etc/red.conf no es válido\n"); return 1; }
        return aplicar(&c);
    }
    return uso();
}
