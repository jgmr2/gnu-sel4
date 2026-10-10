/* Cliente DHCP de netstack (dhcp.c) */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { DHCP_OFF, DHCP_INIT_REBOOT, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING };

/* Direcciones en orden de red; tiempos en segundos */
extern struct dhcp_state {
    int state;
    uint32_t ip, mask, gw, dns[2], server, offer;
    uint32_t lease, t1;
    uint64_t bound_ns;
} dhcp;

void dhcp_start(const uint8_t mac[6], uint32_t preferred_ip);   /* 0 = sin preferencia */
bool dhcp_input(const uint8_t *frame, size_t len);              /* true: era para el cliente DHCP */
void dhcp_tick(void);

/* Los da netstack.c */
void dhcp_send_frame(const void *frame, size_t len);
void dhcp_bound(bool first);                                    /* concesión obtenida o renovada */
