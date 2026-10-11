/* netstack's DHCP client (dhcp.c) */
/* SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { DHCP_OFF, DHCP_INIT_REBOOT, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING,
       DHCP_STATIC };                                           /* = NET_DHCP_* */

/* Addresses in network byte order; times in seconds */
extern struct dhcp_state {
    int state;
    uint32_t ip, mask, gw, dns[2], server, offer;
    uint32_t lease, t1;
    uint64_t bound_ns;
} dhcp;

void dhcp_start(const uint8_t mac[6], uint32_t preferred_ip);   /* 0 = no preference */
void dhcp_stop(void);                                           /* static address: no more DHCP */
bool dhcp_input(const uint8_t *frame, size_t len);              /* true: it was for the DHCP client */
void dhcp_tick(void);

/* Provided by netstack.c */
void dhcp_send_frame(const void *frame, size_t len);
void dhcp_bound(bool first);                                    /* lease obtained or renewed */
