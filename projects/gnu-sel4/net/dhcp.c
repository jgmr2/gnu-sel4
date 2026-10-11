/*
 * netstack's DHCP client (RFC 2131), at the Ethernet frame level: it sits in front
 * of the FreeBSD stack, which is only configured once there is a lease (so there is
 * no need for an interface with 0.0.0.0, nor for BPF).
 *
 *   INIT-REBOOT  asks for the preferred address (QEMU: 10.0.2.16, like a client that
 *                remembers its lease); if the server says NAK, it moves to INIT
 *   INIT         DISCOVER -> OFFER -> REQUEST -> ACK = BOUND
 *   BOUND        halfway through the lease (T1) it renews with REQUEST
 *
 * Retries every 2 s (messages get lost silently). Addresses are in network byte
 * order, as in the packet.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "dhcp.h"
#include "netstack.h"

struct dhcp_state dhcp;

static uint8_t mac[6];
static uint32_t xid, preferred;
static uint64_t next_send;                /* next retry (ns) */
static int tries;

#define RETRY_NS    2000000000ULL
#define SEC         1000000000ULL

enum { DISCOVER = 1, OFFER = 2, REQUEST = 3, ACK = 5, NAK = 6 };

/* ---------------- send ---------------- */

static uint16_t ip_checksum(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 1 < n; i += 2) s += (uint32_t)p[i] << 8 | p[i + 1];
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return ~s;
}

static uint8_t *opt(uint8_t *o, uint8_t code, uint8_t len, const void *data)
{
    *o++ = code;
    *o++ = len;
    memcpy(o, data, len);
    return o + len;
}

static void send(int type, uint32_t ciaddr, uint32_t req_ip, uint32_t server)
{
    uint8_t f[342];
    memset(f, 0, sizeof f);
    /* Ethernet: to broadcast */
    memset(f, 0xff, 6);
    memcpy(f + 6, mac, 6);
    f[12] = 0x08;
    uint8_t *ip = f + 14, *udp = ip + 20, *b = udp + 8;
    /* BOOTP */
    b[0] = 1;                                  /* BOOTREQUEST */
    b[1] = 1;                                  /* Ethernet */
    b[2] = 6;
    memcpy(b + 4, &xid, 4);
    memcpy(b + 12, &ciaddr, 4);
    memcpy(b + 28, mac, 6);                    /* chaddr */
    uint8_t *o = b + 236;
    *o++ = 99; *o++ = 130; *o++ = 83; *o++ = 99;   /* magic cookie */
    uint8_t t = type;
    o = opt(o, 53, 1, &t);
    if (req_ip) o = opt(o, 50, 4, &req_ip);
    if (server) o = opt(o, 54, 4, &server);
    static const uint8_t params[] = { 1, 3, 6, 15, 51, 58, 59 };
    o = opt(o, 55, sizeof params, params);
    o = opt(o, 12, 8, "gnu-sel4");
    *o++ = 255;
    size_t blen = o - b;
    if (blen < 300) blen = 300;                /* minimum BOOTP size */
    size_t ulen = 8 + blen, ilen = 20 + ulen;
    /* UDP 68 -> 67 (checksum 0: optional in IPv4) */
    udp[1] = 68; udp[3] = 67;
    udp[4] = ulen >> 8; udp[5] = ulen;
    /* IPv4 0.0.0.0 -> 255.255.255.255 */
    ip[0] = 0x45;
    ip[2] = ilen >> 8; ip[3] = ilen;
    ip[8] = 64;
    ip[9] = 17;
    memset(ip + 16, 0xff, 4);
    if (ciaddr) memcpy(ip + 12, &ciaddr, 4);
    uint16_t cs = ip_checksum(ip, 20);
    ip[10] = cs >> 8; ip[11] = cs;
    dhcp_send_frame(f, 14 + ilen);
}

/* ---------------- receive ---------------- */

static void parse_options(const uint8_t *o, const uint8_t *end, int *type, uint32_t *server, struct dhcp_state *l)
{
    if (end - o < 4 || o[0] != 99 || o[1] != 130 || o[2] != 83 || o[3] != 99) return;
    for (o += 4; o < end && *o != 255; ) {
        if (*o == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) break;
        uint8_t code = o[0], len = o[1];
        const uint8_t *d = o + 2;
        uint32_t v = 0;
        if (len >= 4) memcpy(&v, d, 4);
        switch (code) {
        case 53: if (len) *type = d[0]; break;
        case 54: *server = v; break;
        case 1:  l->mask = v; break;
        case 3:  l->gw = v; break;
        case 6:
            l->dns[0] = v;
            if (len >= 8) memcpy(&l->dns[1], d + 4, 4);
            break;
        case 51: l->lease = __builtin_bswap32(v); break;
        case 58: l->t1 = __builtin_bswap32(v); break;
        }
        o += 2 + len;
    }
}

/* Checks whether the frame is a DHCP reply for us; if it is, consumes it */
bool dhcp_input(const uint8_t *f, size_t len)
{
    if (dhcp.state == DHCP_OFF || dhcp.state == DHCP_STATIC) return false;
    if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00) return false;
    const uint8_t *ip = f + 14;
    size_t ihl = (ip[0] & 15) * 4;
    if (ip[9] != 17 || len < 14 + ihl + 8 + 240) return false;
    const uint8_t *udp = ip + ihl, *b = udp + 8;
    if (udp[2] != 0 || udp[3] != 68) return false;                /* destination port 68 */
    if (b[0] != 2 || memcmp(b + 4, &xid, 4) || memcmp(b + 28, mac, 6)) return false;

    struct dhcp_state l = { 0 };
    int type = 0;
    uint32_t server = 0, yiaddr;
    memcpy(&yiaddr, b + 16, 4);
    parse_options(b + 236, f + len, &type, &server, &l);
    uint64_t now = netstack_now_ns();

    if (type == OFFER && dhcp.state == DHCP_SELECTING) {
        dhcp.state = DHCP_REQUESTING;
        dhcp.offer = yiaddr;
        dhcp.server = server;
        tries = 0;
        send(REQUEST, 0, yiaddr, server);
        next_send = now + RETRY_NS;
    } else if (type == ACK && dhcp.state != DHCP_BOUND) {
        bool first = !dhcp.ip;
        dhcp.state = DHCP_BOUND;
        dhcp.ip = yiaddr;
        dhcp.server = server ? server : dhcp.server;
        dhcp.mask = l.mask ? l.mask : 0x00ffffff;
        dhcp.gw = l.gw;
        dhcp.dns[0] = l.dns[0];
        dhcp.dns[1] = l.dns[1];
        dhcp.lease = l.lease ? l.lease : 3600;
        dhcp.t1 = l.t1 ? l.t1 : dhcp.lease / 2;
        dhcp.bound_ns = now;
        next_send = now + (uint64_t)dhcp.t1 * SEC;
        tries = 0;
        dhcp_bound(first);
    } else if (type == NAK && dhcp.state != DHCP_BOUND) {
        netstack_log("netstack: DHCP: the server rejected the address; asking for a new one\n");
        dhcp.state = DHCP_SELECTING;
        xid++;
        tries = 0;
        send(DISCOVER, 0, 0, 0);
        next_send = now + RETRY_NS;
    }
    return true;
}

/* ---------------- time ---------------- */

void dhcp_tick(void)
{
    uint64_t now = netstack_now_ns();
    if (!next_send || now < next_send) return;
    tries++;
    switch (dhcp.state) {
    case DHCP_INIT_REBOOT:
        if (tries > 2) {                       /* nobody answers for that address: start from scratch */
            dhcp.state = DHCP_SELECTING;
            send(DISCOVER, 0, 0, 0);
        } else {
            send(REQUEST, 0, preferred, 0);
        }
        next_send = now + RETRY_NS;
        break;
    case DHCP_SELECTING:
        send(DISCOVER, 0, 0, 0);
        next_send = now + RETRY_NS;
        break;
    case DHCP_REQUESTING:
        if (tries > 4) { dhcp.state = DHCP_SELECTING; tries = 0; }
        send(dhcp.state == DHCP_SELECTING ? DISCOVER : REQUEST, 0,
             dhcp.state == DHCP_SELECTING ? 0 : dhcp.offer, dhcp.server);
        next_send = now + RETRY_NS;
        break;
    case DHCP_BOUND:                           /* T1: renew (broadcast, as in REBINDING) */
        dhcp.state = DHCP_RENEWING;
        tries = 0;
        /* fallthrough */
    case DHCP_RENEWING:
        send(REQUEST, dhcp.ip, 0, 0);
        if (now >= dhcp.bound_ns + (uint64_t)dhcp.lease * SEC)
            netstack_log("netstack: DHCP: the lease expired without being renewed; still using the address\n");
        next_send = now + (tries < 5 ? RETRY_NS : 30 * SEC);
        break;
    }
}

void dhcp_stop(void)
{
    next_send = 0;
    dhcp.state = DHCP_OFF;
}

void dhcp_start(const uint8_t m[6], uint32_t preferred_ip)
{
    memcpy(mac, m, 6);
    xid = (uint32_t)netstack_now_ns() ^ ((uint32_t)m[4] << 24 | (uint32_t)m[5] << 16);
    preferred = preferred_ip;
    dhcp.state = preferred ? DHCP_INIT_REBOOT : DHCP_SELECTING;
    if (preferred) send(REQUEST, 0, preferred, 0);
    else send(DISCOVER, 0, 0, 0);
    next_send = netstack_now_ns() + RETRY_NS;
}
