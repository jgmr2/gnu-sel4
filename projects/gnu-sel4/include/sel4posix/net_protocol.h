/*
 * posix_server -> netstack protocol (FreeBSD TCP/IP stack).
 *
 * posix_server calls over PPC: MR0 = operation, MR1..MR4 = arguments; the
 * data goes in net_buf (shared memory). Reply: MR0 = result (>= 0) or Linux
 * -errno; MR1 = length of the returned address or option.
 *
 * Sockets: the number NET_OP_SOCKET/NET_OP_ACCEPT returns. Everything uses the
 * Linux ABI (families, types, options, struct sockaddr): F-Stack translates it.
 * Inside netstack sockets never block (-EAGAIN, -EINPROGRESS); posix_server
 * does the waiting. netstack notifies posix_server when there is activity on
 * some socket, so that it retries whatever was waiting.
 *
 * Layout of net_buf (and of the process's shared buffer in these calls):
 *   [0, NET_ADDR_OFF)          data (send/recv) or option (setsockopt/getsockopt)
 *   [NET_ADDR_OFF, +128)       address (bind, connect, accept, sendto, recvfrom...)
 */
#pragma once
#include <stdint.h>

#define NET_BUF_SIZE   0x10000
#define NET_ADDR_OFF   (NET_BUF_SIZE - 128)
#define NET_ADDR_MAX   128
#define NET_DATA_MAX   NET_ADDR_OFF

enum {
    NET_OP_SOCKET = 1,     /* domain, type, protocol                  -> socket */
    NET_OP_BIND,           /* s, address length */
    NET_OP_CONNECT,        /* s, address length */
    NET_OP_LISTEN,         /* s, backlog */
    NET_OP_ACCEPT,         /* s                                        -> socket; address, MR1 */
    NET_OP_SEND,           /* s, len, flags, address length (0 = connected) -> bytes */
    NET_OP_RECV,           /* s, len, flags                            -> bytes; address, MR1 */
    NET_OP_SHUTDOWN,       /* s, how */
    NET_OP_CLOSE,          /* s */
    NET_OP_SETSOCKOPT,     /* s, level, name, length */
    NET_OP_GETSOCKOPT,     /* s, level, name, length                  -> option, MR1 */
    NET_OP_GETSOCKNAME,    /* s                                        -> address, MR1 */
    NET_OP_GETPEERNAME,    /* s                                        -> address, MR1 */
    NET_OP_POLL,           /* s, events (POLLIN...)                   -> revents */
    NET_OP_FIONREAD,       /* s                                        -> bytes to read */
    NET_OP_INFO,           /*                                          -> struct net_info in net_buf */
    NET_OP_CONFIG,         /* struct net_cfg in net_buf                (DHCP or static address) */
};

/* Interface configuration (NET_OP_CONFIG; programs request it with the SIOCDEVPRIVATE
 * ioctl on an AF_INET socket, see apps/netcfg). Addresses in network byte order. */
#define NET_SIOC_CONFIG 0x89F0          /* Linux SIOCDEVPRIVATE */
enum { NET_CFG_DHCP, NET_CFG_STATIC };
struct net_cfg {
    uint32_t mode;                      /* NET_CFG_* */
    uint32_t ip, mask, gw, dns[2];
};

/* Interface status (NET_OP_INFO). Addresses in network byte order. netstack notifies
 * posix_server on its lease channel every time it obtains or renews a lease. */
enum { NET_DHCP_OFF, NET_DHCP_INIT_REBOOT, NET_DHCP_SELECTING, NET_DHCP_REQUESTING,
       NET_DHCP_BOUND, NET_DHCP_RENEWING, NET_DHCP_STATIC };
struct net_info {
    uint32_t state;              /* NET_DHCP_* (= DHCP_* in net/dhcp.h) */
    uint32_t ip, mask, gw, dns[2], server;
    uint32_t lease, remaining;   /* seconds */
    uint8_t mac[6];
};
