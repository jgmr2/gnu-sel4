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
 * Dirección fija por ahora (red de usuario de QEMU): 10.0.2.16/24, puerta 10.0.2.2.
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

/* De la parte de FreeBSD (exportadas en lib/ff_api.symlist) */
extern int ff_freebsd_init(void);
extern void ff_hardclock(void);

__attribute__((__section__(".timer_client_config"))) timer_client_config_t timer_config;
__attribute__((__section__(".net_client_config"))) net_client_config_t net_config;

#define SERVER_CH 50                  /* canal con posix_server (= meta.py) */
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
    flush_notifications();                      /* lo enviado sale ya hacia el driver */
    return microkit_msginfo_new(0, 2);
}

/* ---------------- Microkit ---------------- */

static void tick(void)
{
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

    static char ifname[] = "eth0", addr[] = "10.0.2.16", mask[] = "255.255.255.0",
                bcast[] = "10.0.2.255", gw[] = "10.0.2.2";
    port_cfg.name = ifname;
    port_cfg.ifname = ifname;
    port_cfg.addr = addr;
    port_cfg.netmask = mask;
    port_cfg.broadcast = bcast;
    port_cfg.gateway = gw;
    memcpy(port_cfg.mac, &net_config.mac_addr, 6);
    if (!ff_veth_attach(&port_cfg)) {
        netstack_log("netstack: ff_veth_attach falló\n");
        return;
    }
    netstack_log("netstack: pila TCP/IP de FreeBSD lista: eth0 %s/24, puerta %s\n", addr, gw);

    kq = ff_kqueue();
    sddf_timer_set_timeout(timer_config.driver_id, NS_PER_TICK);
    flush_notifications();
}

void notified(microkit_channel ch)
{
    if (ch == net_config.rx.id) process_rx();
    else if (ch == timer_config.driver_id) tick();
    check_activity();
    flush_notifications();
}
