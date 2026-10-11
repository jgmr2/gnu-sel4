/*
 * A network driver with no network card: live systems on hardware we have no driver for yet
 * (make PLATFORM=...). It speaks the sDDF driver protocol to the network virtualisers, never
 * receives anything and hands every transmitted buffer straight back, so netstack and the VM
 * start normally and simply see a link with nobody on it.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdbool.h>
#include <stdint.h>
#include <os/sddf.h>
#include <sddf/resources/device.h>
#include <sddf/network/queue.h>
#include <sddf/network/config.h>

__attribute__((__section__(".device_resources"))) device_resources_t device_resources;
__attribute__((__section__(".net_driver_config"))) net_driver_config_t config;

static net_queue_handle_t rx_queue, tx_queue;

/* Everything the virtualiser wants sent goes back to it as done */
static void tx_return(void)
{
    bool returned = false;
    for (;;) {
        net_buff_desc_t buffer;
        while (!net_queue_empty_active(&tx_queue)) {
            if (net_dequeue_active(&tx_queue, &buffer)) break;
            buffer.len = 0;
            net_enqueue_free(&tx_queue, buffer);
            returned = true;
        }
        net_request_signal_active(&tx_queue);
        if (net_queue_empty_active(&tx_queue)) break;
        net_cancel_signal_active(&tx_queue);
    }
    if (returned && net_require_signal_free(&tx_queue)) {
        net_cancel_signal_free(&tx_queue);
        sddf_notify(config.virt_tx.id);
    }
}

void init(void)
{
    net_queue_init(&rx_queue, config.virt_rx.free_queue.vaddr, config.virt_rx.active_queue.vaddr,
                   config.virt_rx.num_buffers);
    net_queue_init(&tx_queue, config.virt_tx.free_queue.vaddr, config.virt_tx.active_queue.vaddr,
                   config.virt_tx.num_buffers);
    tx_return();
}

void notified(sddf_channel ch)
{
    if (ch == config.virt_tx.id) tx_return();
    /* virt_rx: free buffers to fill; nothing ever arrives, they just stay with us */
}
