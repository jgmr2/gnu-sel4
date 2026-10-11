/*
 * A minimal virtio-vsock device for the bridge to GNU/seL4.
 *
 * The VM's agent connects to the host (CID 2) on port VSOCK_BRIDGE_PORT; the bytes of that one
 * stream connection travel through the same queues as the old /dev/hvc1 console (posix_server
 * sees no difference). vsock carries up to 64 KiB per packet where the tty layer behind hvc1
 * handed over 2 KiB at a time: far fewer VM exits.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <libvmm/virq.h>
#include <libvmm/virtio/virtio.h>
#include <sddf/serial/queue.h>

#define VIRTIO_DEVICE_ID_VSOCK 19
#define VSOCK_NUM_VIRTQ        3        /* rx, tx, event */
#define VSOCK_HOST_CID         2
#define VSOCK_GUEST_CID        3
#define VSOCK_BRIDGE_PORT      5000     /* BR_VSOCK_PORT in include/sel4posix/bridge.h */

struct virtio_vsock_device {
    struct virtio_device virtio_device;
    struct virtio_queue_handler vqs[VSOCK_NUM_VIRTQ];
    struct { uint64_t guest_cid; } __attribute__((packed)) config;
    serial_queue_handle_t *to_vm;       /* posix_server produces, the VM consumes */
    serial_queue_handle_t *from_vm;     /* the VM produces, posix_server consumes */
    int ch;                             /* channel to posix_server */

    /* The one connection */
    bool connected;
    uint32_t guest_port;
    uint32_t peer_buf_alloc, peer_fwd_cnt;  /* the guest's receive credit */
    uint32_t sent;                      /* payload bytes sent to the guest */
    uint32_t fwd_told;                  /* our fwd_cnt as last told to the guest */
    /* Control packets waiting for a receive buffer */
    bool want_response, want_credit;
    bool want_rst;
    uint32_t rst_port;
};

bool virtio_pci_vsock_init(struct virtio_vsock_device *vs, uint16_t pci_bus, uint16_t pci_dev,
                           irq_routing_info_t irq_routing_info, serial_queue_handle_t *to_vm,
                           serial_queue_handle_t *from_vm, int ch);

/* posix_server notified: data to send to the VM, or room it made in from_vm */
bool virtio_vsock_notified(struct virtio_vsock_device *vs);
