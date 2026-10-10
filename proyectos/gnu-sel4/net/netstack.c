/*
 * netstack: la pila TCP/IP de FreeBSD (de F-Stack, third_party/f-stack) como
 * componente de seL4, cliente de la red de sDDF (con su propio copiador).
 *
 * F-Stack normalmente toma los paquetes de DPDK (lib/ff_dpdk_if.c); aquí ese
 * papel lo hace este archivo:
 *   recepción  cola rx de sDDF -> copia a un búfer propio -> ff_veth_process_packet
 *   envío      ff_dpdk_if_send (lo llama la interfaz de FreeBSD) -> cola tx de sDDF
 *   reloj      temporizador de sDDF cada 1/hz s -> ff_hardclock (timers de TCP, ARP...)
 *
 * Los programas la usan a través de posix_server (include/sel4posix/net_protocol.h):
 * protected() atiende sus llamadas y un kqueue de FreeBSD dice cuándo avisarle de
 * actividad en los sockets.
 *
 * Dirección: por DHCP (dhcp.c, antes de la pila); la interfaz de FreeBSD se crea con
 * la primera concesión. NETSTACK_IP_PREFERIDA (QEMU: 10.0.2.16) se pide primero.
 */
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <microkit.h>
#include <sddf/network/queue.h>
#include <sddf/network/config.h>
#include <sddf/timer/client.h>
#include <sddf/timer/config.h>
#include "ff_api.h"
#include "ff_config.h"
#include "ff_veth.h"
#include "ff_host_interface.h"
#include <sel4posix/net_protocol.h>
#include "netstack.h"
#include "dhcp.h"
#include <arpa/inet.h>

/* De la parte de FreeBSD (exportadas en lib/ff_api.symlist) */
extern int ff_freebsd_init(void);
extern void ff_hardclock(void);
extern void ff_swi_net_excute(void);   /* cola diferida de la pila (loopback, 127.0.0.1) */

__attribute__((__section__(".timer_client_config"))) timer_client_config_t timer_config;
__attribute__((__section__(".net_client_config"))) net_client_config_t net_config;

#define SERVER_CH 50                  /* canal con posix_server (= meta.py) */
#define LEASE_CH  51                  /* aviso de concesión DHCP a posix_server (= meta.py) */
static bool attached;                 /* la interfaz de FreeBSD existe (hay concesión) */
uintptr_t net_buf;                    /* compartido con posix_server (setvar en meta.py) */

#define HZ 100
#define NS_PER_TICK (1000000000ULL / HZ)

static net_queue_handle_t rx_q, tx_q;
static bool notify_rx, notify_tx;

uint64_t netstack_now_ns(void) { return sddf_timer_time_now(timer_config.driver_id); }

/* ---------------- configuración de F-Stack ---------------- */

struct ff_config ff_global_cfg;
static struct ff_port_cfg port_cfg;

/* Lo que FreeBSD necesita de la interfaz (ff_veth.c la registra con nosotros) */
struct ff_dpdk_if_context {
    void *sc;
    void *ifp;
};
static struct ff_dpdk_if_context if_ctx;

struct ff_dpdk_if_context *ff_dpdk_register_if(void *sc, void *ifp, struct ff_port_cfg *cfg)
{
    (void)cfg;
    if_ctx.sc = sc;
    if_ctx.ifp = ifp;
    return &if_ctx;
}

void ff_dpdk_deregister_if(struct ff_dpdk_if_context *ctx) { (void)ctx; }

/* ---------------- envío ---------------- */

int ff_dpdk_if_send(struct ff_dpdk_if_context *ctx, void *m, int total)
{
    (void)ctx;
    net_buff_desc_t b;
    if (total > NET_BUFFER_SIZE || net_dequeue_free(&tx_q, &b)) {
        ff_mbuf_free(m);                       /* sin búfer libre: se descarta (TCP reenvía) */
        return -1;
    }
    ff_mbuf_copydata(m, (void *)(net_config.tx_data.vaddr + b.io_or_offset), 0, total);
    ff_mbuf_free(m);
    b.len = total;
    net_enqueue_active(&tx_q, b);
    notify_tx = true;
    return 0;
}

/* ---------------- recepción ---------------- */

/* Cada paquete se copia a un búfer propio: así FreeBSD puede retenerlo (cola de
 * reensamblado de TCP...) sin agotar los búferes de sDDF */
void ff_dpdk_pktmbuf_free(void *m) { free(m); }

static void process_rx(void)
{
    bool again = true;
    while (again) {
        while (!net_queue_empty_active(&rx_q)) {
            net_buff_desc_t b;
            net_dequeue_active(&rx_q, &b);
            uint16_t len = b.len;
            void *copy = malloc(len);
            if (copy) memcpy(copy, (void *)(net_config.rx_data.vaddr + b.io_or_offset), len);
            b.len = 0;
            net_enqueue_free(&rx_q, b);
            notify_rx = true;
            if (!copy) continue;
            if (dhcp_input(copy, len) || !attached) {   /* respuestas DHCP; sin interfaz aún: nada más */
                free(copy);
                continue;
            }
            void *m = ff_mbuf_gethdr(copy, len, copy, len, 0);
            if (!m) {
                free(copy);
                continue;
            }
            ff_veth_process_packet(if_ctx.ifp, m);
        }
        net_request_signal_active(&rx_q);
        again = false;
        if (!net_queue_empty_active(&rx_q)) {
            net_cancel_signal_active(&rx_q);
            again = true;
        }
    }
}

static void flush_notifications(void)
{
    if (notify_rx && net_require_signal_free(&rx_q)) {
        net_cancel_signal_free(&rx_q);
        microkit_notify(net_config.rx.id);
    }
    notify_rx = false;
    if (notify_tx && net_require_signal_active(&tx_q)) {
        net_cancel_signal_active(&tx_q);
        microkit_notify(net_config.tx.id);
    }
    notify_tx = false;
}

/* ---------------- DHCP ---------------- */

void dhcp_send_frame(const void *frame, size_t len)
{
    net_buff_desc_t b;
    if (len > NET_BUFFER_SIZE || net_dequeue_free(&tx_q, &b)) return;   /* se reintenta */
    memcpy((void *)(net_config.tx_data.vaddr + b.io_or_offset), frame, len);
    b.len = len;
    net_enqueue_active(&tx_q, b);
    notify_tx = true;
}


/* ---------------- configuración de la interfaz ---------------- */

/* Estructuras de FreeBSD para sus ioctl (sockaddr_in con sin_len) */
struct bsd_sin { uint8_t len, family; uint16_t port; uint32_t addr; uint8_t zero[8]; };
struct bsd_ifreq { char name[16]; struct bsd_sin addr; };
struct bsd_aliasreq { char name[16]; struct bsd_sin addr, broad, mask; int vhid; };
#define BSD_SIOCAIFADDR 0x8044692bUL           /* _IOW('i', 43, struct in_aliasreq) */
#define BSD_SIOCDIFADDR 0x80206919UL           /* _IOW('i', 25, struct ifreq) */
#define BSIN(a) { sizeof(struct bsd_sin), AF_INET, 0, (a), { 0 } }

static uint32_t cur_ip, cur_mask, cur_gw;
static int cfg_sock = -1;

static void set_default_route(uint32_t gw)
{
    struct sockaddr_in dst = { .sin_family = AF_INET }, nm = { .sin_family = AF_INET },
                       g = { .sin_family = AF_INET, .sin_addr.s_addr = gw };
    ff_route_ctl(FF_ROUTE_DEL, FF_RTF_GATEWAY, (struct linux_sockaddr *)&dst, NULL, (struct linux_sockaddr *)&nm);
    if (gw) ff_route_ctl(FF_ROUTE_ADD, FF_RTF_GATEWAY, (struct linux_sockaddr *)&dst, (struct linux_sockaddr *)&g,
                         (struct linux_sockaddr *)&nm);
}

/* Pone la dirección de eth0: la primera vez crea la interfaz; después la cambia en caliente */
static bool set_addr(uint32_t ip, uint32_t mask, uint32_t gw)
{
    char a[16], m[16], g[16];
    inet_ntop(AF_INET, &ip, a, sizeof a);
    inet_ntop(AF_INET, &mask, m, sizeof m);
    inet_ntop(AF_INET, &gw, g, sizeof g);
    if (!attached) {
        static char ifname[] = "eth0";
        static char sa[16], sm[16], sb[16], sg[16];
        uint32_t b = ip | ~mask;
        memcpy(sa, a, sizeof sa);
        memcpy(sm, m, sizeof sm);
        inet_ntop(AF_INET, &b, sb, sizeof sb);
        memcpy(sg, g, sizeof sg);
        port_cfg.name = port_cfg.ifname = ifname;
        port_cfg.addr = sa;
        port_cfg.netmask = sm;
        port_cfg.broadcast = sb;
        port_cfg.gateway = sg;
        memcpy(port_cfg.mac, &net_config.mac_addr, 6);
        if (!ff_veth_attach(&port_cfg)) {
            netstack_log("netstack: ff_veth_attach falló\n");
            return false;
        }
        attached = true;
        if (!gw) set_default_route(0);
    } else {
        if (cfg_sock < 0) cfg_sock = ff_socket(AF_INET, SOCK_DGRAM, 0);
        bool moved = ip != cur_ip || mask != cur_mask;
        if (moved) {
            struct bsd_ifreq dr = { "eth0", BSIN(cur_ip) };
            ff_ioctl_freebsd(cfg_sock, BSD_SIOCDIFADDR, &dr);
            struct bsd_aliasreq ar = { "eth0", BSIN(ip), BSIN(ip | ~mask), BSIN(mask), 0 };
            if (ff_ioctl_freebsd(cfg_sock, BSD_SIOCAIFADDR, &ar) < 0) {
                netstack_log("netstack: no se pudo poner la dirección %s\n", a);
                return false;
            }
        }
        /* quitar la dirección se lleva las rutas que pasaban por ella: la de omisión se repone */
        if (moved || gw != cur_gw) set_default_route(gw);
    }
    cur_ip = ip;
    cur_mask = mask;
    cur_gw = gw;
    netstack_log("netstack: eth0 %s/%d, puerta %s\n", a, __builtin_popcount(mask), gw ? g : "ninguna");
    return true;
}

void dhcp_bound(bool first)
{
    char dns[16];
    inet_ntop(AF_INET, &dhcp.dns[0], dns, sizeof dns);
    if (first || dhcp.ip != cur_ip || dhcp.mask != cur_mask || dhcp.gw != cur_gw) {
        netstack_log("netstack: DHCP: concesión de %u s, DNS %s\n", dhcp.lease, dns);
        set_addr(dhcp.ip, dhcp.mask, dhcp.gw ? dhcp.gw : dhcp.server);
    }
    microkit_notify(LEASE_CH);                 /* posix_server actualiza /etc/resolv.conf */
}

/* Dirección fija o vuelta a DHCP (NET_OP_CONFIG) */
static long configure(const struct net_cfg *c)
{
    if (c->mode == NET_CFG_FIJA) {
        if (!c->ip || !c->mask) return -EINVAL;
        dhcp_stop();
        dhcp.state = DHCP_STATIC;
        dhcp.ip = c->ip;
        dhcp.mask = c->mask;
        dhcp.gw = c->gw;
        dhcp.dns[0] = c->dns[0];
        dhcp.dns[1] = c->dns[1];
        dhcp.server = dhcp.lease = 0;
        dhcp.bound_ns = 0;
        return set_addr(c->ip, c->mask, c->gw) ? 0 : -EIO;
    }
    if (c->mode == NET_CFG_DHCP) {
        if (dhcp.state != DHCP_STATIC && dhcp.state != DHCP_OFF) return 0;   /* ya en DHCP */
        dhcp_start((const uint8_t *)&net_config.mac_addr, cur_ip);           /* pide la que tiene */
        return 0;
    }
    return -EINVAL;
}

/* ---------------- llamadas de posix_server ---------------- */

static int kq = -1;
static bool activity;

/* Vigila un socket nuevo: el kqueue avisa cuando cambia su estado */
static void watch(int fd)
{
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
    ff_kevent(kq, ev, 2, NULL, 0, NULL);
    int on = 1;
    ff_ioctl(fd, FIONBIO, &on);
}

/* ¿Hubo actividad en algún socket desde la última vez? */
static void check_activity(void)
{
    struct kevent ev[64];
    struct timespec zero = { 0, 0 };
    if (ff_kevent(kq, NULL, 0, ev, 64, &zero) > 0) activity = true;
    if (activity) {
        activity = false;
        microkit_notify(SERVER_CH);
    }
}

/* Banderas MSG_* de Linux que tienen sentido aquí (las demás las resuelve posix_server) */
static int msg_flags(long f) { return f & (0x1 | 0x2 | 0x4); }     /* OOB, PEEK, DONTROUTE */

/* Resultado de una llamada ff_*: -1 + errno -> -errno */
static long RET(long x) { return x < 0 ? -(long)errno : x; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    long op = microkit_mr_get(0), a1 = microkit_mr_get(1), a2 = microkit_mr_get(2);
    long a3 = microkit_mr_get(3), a4 = microkit_mr_get(4);
    char *data = (char *)net_buf;
    struct linux_sockaddr *addr = (struct linux_sockaddr *)(net_buf + NET_ADDR_OFF);
    socklen_t alen = NET_ADDR_MAX;
    long r;
    switch (op) {
    case NET_OP_SOCKET:
        r = ff_socket(a1, a2 & 0xf, a3);              /* sin SOCK_NONBLOCK/CLOEXEC: los lleva posix_server */
        if (r >= 0) watch(r);
        r = RET(r);
        break;
    case NET_OP_BIND:     r = RET(ff_bind(a1, addr, a2)); break;
    case NET_OP_CONNECT:  r = RET(ff_connect(a1, addr, a2)); break;
    case NET_OP_LISTEN:   r = RET(ff_listen(a1, a2)); break;
    case NET_OP_ACCEPT:
        r = ff_accept(a1, addr, &alen);
        if (r >= 0) watch(r);
        r = RET(r);
        microkit_mr_set(1, alen);
        break;
    case NET_OP_SEND:
        if (a2 > NET_DATA_MAX) a2 = NET_DATA_MAX;
        r = RET(a4 ? ff_sendto(a1, data, a2, msg_flags(a3), addr, a4) : ff_send(a1, data, a2, msg_flags(a3)));
        break;
    case NET_OP_RECV:
        if (a2 > NET_DATA_MAX) a2 = NET_DATA_MAX;
        r = RET(ff_recvfrom(a1, data, a2, msg_flags(a3), addr, &alen));
        microkit_mr_set(1, r >= 0 ? alen : 0);
        break;
    case NET_OP_SHUTDOWN: r = RET(ff_shutdown(a1, a2)); break;
    case NET_OP_CLOSE:    r = RET(ff_close(a1)); break;
    case NET_OP_SETSOCKOPT:
        r = RET(ff_setsockopt(a1, a2, a3, data, a4));
        break;
    case NET_OP_GETSOCKOPT: {
        socklen_t olen = a4 < NET_DATA_MAX ? a4 : NET_DATA_MAX;
        r = RET(ff_getsockopt(a1, a2, a3, data, &olen));
        microkit_mr_set(1, olen);
        break;
    }
    case NET_OP_GETSOCKNAME:
    case NET_OP_GETPEERNAME:
        r = RET(op == NET_OP_GETSOCKNAME ? ff_getsockname(a1, addr, &alen) : ff_getpeername(a1, addr, &alen));
        microkit_mr_set(1, alen);
        break;
    case NET_OP_POLL: {
        struct pollfd p = { .fd = a1, .events = a2 };
        r = ff_poll(&p, 1, 0);
        r = r < 0 ? -(long)errno : p.revents;
        break;
    }
    case NET_OP_INFO: {
        struct net_info *ni = (struct net_info *)net_buf;
        memset(ni, 0, sizeof *ni);
        ni->state = dhcp.state;
        ni->ip = attached ? cur_ip : 0;
        ni->mask = cur_mask;
        ni->gw = cur_gw;
        ni->dns[0] = dhcp.dns[0];
        ni->dns[1] = dhcp.dns[1];
        ni->server = dhcp.server;
        ni->lease = dhcp.lease;
        uint64_t end = dhcp.bound_ns + (uint64_t)dhcp.lease * 1000000000ULL, now = netstack_now_ns();
        ni->remaining = dhcp.bound_ns && end > now ? (end - now) / 1000000000ULL : 0;
        memcpy(ni->mac, &net_config.mac_addr, 6);
        r = 0;
        break;
    }
    case NET_OP_CONFIG:
        r = configure((const struct net_cfg *)net_buf);
        break;
    case NET_OP_FIONREAD: {
        int n = 0;
        r = ff_ioctl(a1, FIONREAD, &n);
        r = r < 0 ? -(long)errno : n;
        break;
    }
    default:
        r = -ENOSYS;
    }
    microkit_mr_set(0, r);
    ff_swi_net_excute();                        /* paquetes por lo0 (127.0.0.1, la propia IP) */
    check_activity();
    flush_notifications();                      /* lo enviado sale ya hacia el driver */
    return microkit_msginfo_new(0, 2);
}

/* ---------------- Microkit ---------------- */

static void tick(void)
{
    dhcp_tick();
    ff_hardclock();
    ff_update_current_ts();
    sddf_timer_set_timeout(timer_config.driver_id, NS_PER_TICK);
}

void init(void)
{
    netstack_host_init();

    net_queue_init(&rx_q, net_config.rx.free_queue.vaddr, net_config.rx.active_queue.vaddr,
                   net_config.rx.num_buffers);
    net_queue_init(&tx_q, net_config.tx.free_queue.vaddr, net_config.tx.active_queue.vaddr,
                   net_config.tx.num_buffers);
    net_buffers_init(&tx_q, 0);

    ff_global_cfg.freebsd.hz = HZ;
    ff_global_cfg.freebsd.physmem = NETSTACK_HEAP_SIZE;
    ff_global_cfg.freebsd.mem_size = NETSTACK_HEAP_SIZE >> 20;
    ff_update_current_ts();
    if (ff_freebsd_init() < 0) {
        netstack_log("netstack: ff_freebsd_init falló\n");
        return;
    }

    kq = ff_kqueue();
    uint32_t pref = 0;
#ifdef NETSTACK_IP_PREFERIDA
    inet_pton(AF_INET, NETSTACK_IP_PREFERIDA, &pref);
#endif
    dhcp_start((const uint8_t *)&net_config.mac_addr, pref);
    netstack_log("netstack: pila TCP/IP de FreeBSD lista; pidiendo dirección por DHCP\n");
    sddf_timer_set_timeout(timer_config.driver_id, NS_PER_TICK);
    flush_notifications();
}

void notified(microkit_channel ch)
{
    if (ch == net_config.rx.id) process_rx();
    else if (ch == timer_config.driver_id) tick();
    ff_swi_net_excute();
    check_activity();
    flush_notifications();
}
