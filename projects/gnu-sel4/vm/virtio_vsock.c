/*
 * A minimal virtio-vsock device (virtio 1.2, section 5.10) for the bridge to GNU/seL4: one
 * stream connection, from the VM's agent to the host's port VSOCK_BRIDGE_PORT. See
 * virtio_vsock.h.
 *
 * Credit (flow control): what we advertise to the guest is the room in from_vm. Its fwd_cnt is
 * posix_server's position in that queue, so the guest never sends more than fits, and every
 * packet it sends can be copied whole. Towards the guest we respect the credit it advertises.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>
#include <microkit.h>
#include <libvmm/util/util.h>
#include <libvmm/virtio/config.h>
#include <libvmm/virtio/pci.h>
#include <libvmm/virtio/virtio.h>
#include "virtio_vsock.h"

#define RX_VQ    0
#define TX_VQ    1
#define EVENT_VQ 2

#define VSOCK_TYPE_STREAM 1
enum {
    OP_REQUEST = 1, OP_RESPONSE, OP_RST, OP_SHUTDOWN, OP_RW, OP_CREDIT_UPDATE, OP_CREDIT_REQUEST,
};

struct vsock_hdr {
    uint64_t src_cid, dst_cid;
    uint32_t src_port, dst_port;
    uint32_t len;
    uint16_t type, op;
    uint32_t flags;
    uint32_t buf_alloc, fwd_cnt;
} __attribute__((packed));

static inline struct virtio_vsock_device *state(struct virtio_device *dev)
{
    return (struct virtio_vsock_device *)dev->device_data;
}

/* ---------------- device registers ---------------- */

static void regs_init(struct virtio_device *dev)
{
    dev->regs.device_id = VIRTIO_DEVICE_ID_VSOCK;
    dev->regs.vendor_id = VIRTIO_DEV_VENDOR_ID;
}

static void vsock_reset(struct virtio_device *dev)
{
    for (int i = 0; i < dev->num_vqs; i++) {
        dev->vqs[i].ready = false;
        dev->vqs[i].last_idx = 0;
        dev->vqs[i].virtq.avail_gpa = 0;
        dev->vqs[i].virtq.used_gpa = 0;
        dev->vqs[i].virtq.desc_gpa = 0;
        dev->vqs[i].virtq.num = 0;
    }
    virtio_set_interrupt_status(dev, false, false);
    memset(&dev->regs, 0, sizeof(virtio_device_regs_t));
    regs_init(dev);
    struct virtio_vsock_device *vs = state(dev);
    vs->connected = false;
    vs->want_response = vs->want_credit = vs->want_rst = false;
}

static bool get_device_features(struct virtio_device *dev, uint32_t *features)
{
    *features = dev->regs.device_features_sel == 1 ? BIT_HIGH(VIRTIO_F_VERSION_1) : 0;
    return true;
}

static bool set_driver_features(struct virtio_device *dev, uint32_t features)
{
    bool ok = dev->regs.driver_features_sel == 0 ? features == 0
            : dev->regs.driver_features_sel == 1 ? features == BIT_HIGH(VIRTIO_F_VERSION_1) : true;
    if (ok) {
        dev->regs.driver_features = features;
        dev->features_happy = 1;
    }
    return ok;
}

static bool get_device_config(struct virtio_device *dev, uint32_t offset, uint32_t *val)
{
    struct virtio_vsock_device *vs = state(dev);
    if (offset + 4 > sizeof vs->config) {
        *val = 0;
        return true;
    }
    memcpy(val, (uint8_t *)&vs->config + offset, 4);
    return true;
}

static bool set_device_config(struct virtio_device *dev, uint32_t offset, uint32_t val)
{
    (void)dev; (void)offset; (void)val;
    return true;                        /* guest_cid is read-only */
}

/* ---------------- towards the guest (receive queue) ---------------- */

static uint32_t from_vm_fwd_cnt(struct virtio_vsock_device *vs) { return vs->from_vm->queue->head; }

static void fill_hdr(struct virtio_vsock_device *vs, struct vsock_hdr *h, uint16_t op, uint32_t port, uint32_t len)
{
    memset(h, 0, sizeof *h);
    h->src_cid = VSOCK_HOST_CID;
    h->dst_cid = VSOCK_GUEST_CID;
    h->src_port = VSOCK_BRIDGE_PORT;
    h->dst_port = port;
    h->len = len;
    h->type = VSOCK_TYPE_STREAM;
    h->op = op;
    h->buf_alloc = vs->from_vm->capacity;
    h->fwd_cnt = from_vm_fwd_cnt(vs);
    vs->fwd_told = h->fwd_cnt;
}

/* Fills the guest's receive buffers: control packets first, then data within its credit.
 * Returns true if something was delivered (the caller raises the interrupt) */
static bool deliver(struct virtio_vsock_device *vs)
{
    virtio_queue_handler_t *vq = &vs->virtio_device.vqs[RX_VQ];
    if (!vq->ready) return false;
    bool delivered = false;
    uint16_t head;
    while (virtio_virtq_peek_avail(vq, &head)) {
        struct vsock_hdr h;
        uint64_t room = virtio_desc_chain_payload_len(vq, head);
        uint32_t n = 0;
        if (vs->want_rst) {
            fill_hdr(vs, &h, OP_RST, vs->rst_port, 0);
            vs->want_rst = false;
        } else if (vs->want_response) {
            fill_hdr(vs, &h, OP_RESPONSE, vs->guest_port, 0);
            vs->want_response = false;
        } else if (vs->connected && serial_queue_length_consumer(vs->to_vm)) {
            uint32_t credit = vs->peer_buf_alloc - (vs->sent - vs->peer_fwd_cnt);
            if (credit == 0 || credit > vs->peer_buf_alloc || room <= sizeof h) break;
            n = serial_queue_length_consumer(vs->to_vm);
            if (n > credit) n = credit;
            if (n > room - sizeof h) n = room - sizeof h;
            fill_hdr(vs, &h, OP_RW, vs->guest_port, n);
            vs->want_credit = false;                     /* this packet carries it too */
        } else if (vs->want_credit) {
            fill_hdr(vs, &h, OP_CREDIT_UPDATE, vs->guest_port, 0);
            vs->want_credit = false;
        } else {
            break;
        }
        virtio_virtq_pop_avail(vq, &head);
        virtio_write_data_to_desc_chain(vq, head, sizeof h, 0, (char *)&h);
        uint32_t done = 0;
        while (done < n) {                               /* the queue may wrap: two copies */
            uint32_t chunk = serial_queue_contiguous_length(vs->to_vm);
            if (chunk > n - done) chunk = n - done;
            uint32_t qh = vs->to_vm->queue->head;
            virtio_write_data_to_desc_chain(vq, head, chunk, sizeof h + done,
                                            vs->to_vm->data_region + (qh % vs->to_vm->capacity));
            serial_update_shared_head(vs->to_vm, qh + chunk);
            done += chunk;
        }
        vs->sent += n;
        virtio_virtq_add_used(vq, head, sizeof h + n);
        delivered = true;
    }
    /* posix_server waits for room in to_vm (rx_waiters in bridge.c) */
    if (serial_require_consumer_signal(vs->to_vm)) {
        serial_cancel_consumer_signal(vs->to_vm);
        microkit_notify(vs->ch);
    }
    return delivered;
}

/* ---------------- from the guest (transmit queue) ---------------- */

/* Copies a packet's payload into from_vm (the credit guarantees it fits) */
static bool take_payload(struct virtio_vsock_device *vs, virtio_queue_handler_t *vq, uint16_t head, uint32_t len)
{
    serial_queue_handle_t *q = vs->from_vm;
    if (serial_queue_free(q) < len) return false;
    uint32_t done = 0;
    while (done < len) {
        uint32_t tail = q->queue->tail;
        uint32_t chunk = serial_queue_contiguous_free(q);
        if (chunk > len - done) chunk = len - done;
        virtio_read_data_from_desc_chain(vq, head, chunk, sizeof(struct vsock_hdr) + done,
                                         q->data_region + (tail % q->capacity));
        serial_update_shared_tail(q, tail + chunk);
        done += chunk;
    }
    return true;
}

static bool handle_tx(struct virtio_vsock_device *vs, bool *to_posix)
{
    virtio_queue_handler_t *vq = &vs->virtio_device.vqs[TX_VQ];
    if (!vq->ready) return false;
    bool used = false;
    uint16_t head;
    while (virtio_virtq_peek_avail(vq, &head)) {
        struct vsock_hdr h;
        if (virtio_desc_chain_payload_len(vq, head) < sizeof h) {
            virtio_virtq_pop_avail(vq, &head);
            virtio_virtq_add_used(vq, head, 0);
            used = true;
            continue;
        }
        virtio_read_data_from_desc_chain(vq, head, sizeof h, 0, (char *)&h);
        bool ours = h.dst_cid == VSOCK_HOST_CID && h.dst_port == VSOCK_BRIDGE_PORT && h.type == VSOCK_TYPE_STREAM;
        if (ours && (!vs->connected || h.src_port == vs->guest_port)) {
            vs->peer_buf_alloc = h.buf_alloc;
            vs->peer_fwd_cnt = h.fwd_cnt;
        }
        if (!ours) {
            if (h.op != OP_RST) { vs->want_rst = true; vs->rst_port = h.src_port; }
        } else if (h.op == OP_REQUEST) {
            /* (re)connection of the agent: the previous one, if any, is gone */
            vs->connected = true;
            vs->guest_port = h.src_port;
            vs->sent = 0;
            vs->want_response = true;
        } else if (!vs->connected || h.src_port != vs->guest_port) {
            if (h.op != OP_RST) { vs->want_rst = true; vs->rst_port = h.src_port; }
        } else if (h.op == OP_RW) {
            if (!take_payload(vs, vq, head, h.len)) {
                serial_request_consumer_signal(vs->from_vm);     /* posix_server will make room */
                break;                                            /* leave the packet for later */
            }
            *to_posix = true;
        } else if (h.op == OP_SHUTDOWN || h.op == OP_RST) {
            vs->connected = false;
            if (h.op == OP_SHUTDOWN) { vs->want_rst = true; vs->rst_port = h.src_port; }
        } else if (h.op == OP_CREDIT_REQUEST) {
            vs->want_credit = true;
        }                                                         /* OP_CREDIT_UPDATE: noted above */
        virtio_virtq_pop_avail(vq, &head);
        virtio_virtq_add_used(vq, head, 0);
        used = true;
    }
    /* Tell the guest about room posix_server made, before it runs out of credit */
    if (vs->connected && from_vm_fwd_cnt(vs) - vs->fwd_told >= vs->from_vm->capacity / 4) vs->want_credit = true;
    if (*to_posix) serial_request_consumer_signal(vs->from_vm);  /* to hear when it consumes */
    return used;
}

static bool process(struct virtio_vsock_device *vs)
{
    bool to_posix = false;
    bool used = handle_tx(vs, &to_posix);
    bool delivered = deliver(vs);
    if (to_posix) microkit_notify(vs->ch);
    if (used || delivered) {
        virtio_set_interrupt_status(&vs->virtio_device, true, false);
        return virtio_inject_interrupt(&vs->virtio_device);
    }
    return true;
}

static bool queue_notify(struct virtio_device *dev)
{
    return process(state(dev));
}

bool virtio_vsock_notified(struct virtio_vsock_device *vs)
{
    return process(vs);
}

static virtio_device_funs_t functions = {
    .device_reset = vsock_reset,
    .get_device_features = get_device_features,
    .set_driver_features = set_driver_features,
    .get_device_config = get_device_config,
    .set_device_config = set_device_config,
    .queue_notify = queue_notify,
};

bool virtio_pci_vsock_init(struct virtio_vsock_device *vs, uint16_t pci_bus, uint16_t pci_dev,
                           irq_routing_info_t irq_routing_info, serial_queue_handle_t *to_vm,
                           serial_queue_handle_t *from_vm, int ch)
{
    struct virtio_device *dev = &vs->virtio_device;
    dev->transport_type = VIRTIO_TRANSPORT_PCI;
    dev->funs = &functions;
    dev->vqs = vs->vqs;
    dev->num_vqs = VSOCK_NUM_VIRTQ;
    dev->irq_routing_info = irq_routing_info;
    dev->device_data = vs;
    regs_init(dev);
    vs->config.guest_cid = VSOCK_GUEST_CID;
    vs->to_vm = to_vm;
    vs->from_vm = from_vm;
    vs->ch = ch;

    dev->transport.pci.device_id = VIRTIO_PCI_MODERN_BASE_DEVICE_ID + VIRTIO_DEVICE_ID_VSOCK;
    dev->transport.pci.vendor_id = VIRTIO_PCI_VENDOR_ID;
    dev->transport.pci.device_class = PCI_CLASS_COMMUNICATION_OTHER;
    return virtio_pci_register_device(dev, pci_bus, pci_dev, irq_routing_info);
}
