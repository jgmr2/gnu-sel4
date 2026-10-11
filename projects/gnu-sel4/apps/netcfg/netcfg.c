/*
 * netcfg: show and change the GNU/seL4 network configuration (netstack).
 *   netcfg                                          status (like cat /proc/net/dhcp)
 *   netcfg dhcp                                     address via DHCP
 *   netcfg static IP/PREFIX [gateway IP] [dns IP [IP]]   static address
 *   netcfg apply                                    whatever /etc/netcfg.conf says (/etc/rc does it at boot)
 * /etc/netcfg.conf: lines "mode dhcp|static", "address IP/PREFIX", "gateway IP", "dns IP [IP]".
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sel4posix/net_protocol.h>

static int usage(void)
{
    fprintf(stderr, "usage: netcfg | netcfg dhcp | netcfg static IP/PREFIX [gateway IP] [dns IP [IP]] | netcfg apply\n");
    return 2;
}

static int ip_prefix(const char *s, uint32_t *ip, uint32_t *mask)
{
    char b[32];
    snprintf(b, sizeof b, "%s", s);
    char *slash = strchr(b, '/');
    int pre = 24;
    if (slash) {
        *slash = 0;
        pre = atoi(slash + 1);
        if (pre < 1 || pre > 32) return -1;
    }
    *mask = htonl(pre == 32 ? 0xffffffffu : ~(0xffffffffu >> pre));
    return inet_pton(AF_INET, b, ip) == 1 ? 0 : -1;
}

static int apply(struct net_cfg *c)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0 || ioctl(s, NET_SIOC_CONFIG, c) < 0) {
        perror("netcfg");
        return 1;
    }
    close(s);
    return 0;
}

static int show(void)
{
    FILE *f = fopen("/proc/net/dhcp", "r");
    if (!f) { perror("/proc/net/dhcp"); return 1; }
    char l[160];
    while (fgets(l, sizeof l, f)) fputs(l, stdout);
    fclose(f);
    return 0;
}

/* Parses "static IP/P gateway G dns D [D]" (argv) */
static int parse_static(int argc, char **argv, struct net_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->mode = NET_CFG_STATIC;
    if (argc < 1 || ip_prefix(argv[0], &c->ip, &c->mask)) return -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "gateway") && i + 1 < argc) {
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
    if (argc == 1) return show();
    if (!strcmp(argv[1], "dhcp")) return apply(&c);
    if (!strcmp(argv[1], "static")) return parse_static(argc - 2, argv + 2, &c) ? usage() : apply(&c);
    if (!strcmp(argv[1], "apply")) {
        FILE *f = fopen("/etc/netcfg.conf", "r");
        if (!f) return 0;                              /* no file: DHCP (what netstack already does) */
        char l[160], *args[8];
        int n = 0, is_static = 0;
        static char saved[8][40];
        while (fgets(l, sizeof l, f)) {
            char *k = strtok(l, " \t\r\n"), *v;
            if (!k || *k == '#') continue;
            if (!strcmp(k, "mode") && (v = strtok(NULL, " \t\r\n"))) is_static = !strcmp(v, "static");
            else if (!strcmp(k, "address") && (v = strtok(NULL, " \t\r\n")) && n < 1) {
                snprintf(saved[n], 40, "%s", v); args[n] = saved[n]; n++;
            } else if ((!strcmp(k, "gateway") || !strcmp(k, "dns")) && n > 0) {
                for (char *w = k; w && n < 8; w = strtok(NULL, " \t\r\n")) {
                    snprintf(saved[n], 40, "%s", w); args[n] = saved[n]; n++;
                }
            }
        }
        fclose(f);
        if (!is_static) return apply(&c);
        if (parse_static(n, args, &c)) { fprintf(stderr, "netcfg: /etc/netcfg.conf is not valid\n"); return 1; }
        return apply(&c);
    }
    return usage();
}
