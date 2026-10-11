/*
 * Copyright 2026, UNSW
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stddef.h>
#include <stdint.h>
#include <microkit.h>
#include <libvmm/libvmm.h>
#include <sddf/serial/queue.h>
#include <sddf/serial/config.h>
#include <sddf/blk/queue.h>
#include <sddf/blk/config.h>
#include <sddf/network/queue.h>
#include <sddf/network/config.h>
#include <sddf/timer/config.h>
#include "guest_arch_init.h"
#include "virtio_vsock.h"

/* Device slot of the virtio console device on bus 0.
 * Host bridge = 0, ISA bridge = 1 so we must avoid these.
 * Then on Intel, the integrated graphics is conventionally on slot 2 as well...
 * Make sure these two matches what is written in DSDT
 */
#define VIRTIO_CONSOLE_PCI_DEVICE_SLOT 3
#define VIRTIO_CONSOLE_PCI_IOAPIC_PIN 9

#define VIRTIO_NET_PCI_DEVICE_SLOT 4
#define VIRTIO_NET_PCI_IOAPIC_PIN 10

#define VIRTIO_BLK_PCI_DEVICE_SLOT 5
#define VIRTIO_BLK_PCI_IOAPIC_PIN 11

/* The bridge to GNU/seL4: a vsock device (vm/virtio_vsock.c) */
#define BRIDGE_VSOCK_PCI_DEVICE_SLOT 6
#define BRIDGE_VSOCK_PCI_IOAPIC_PIN 12

/* Data from sdfgen */
__attribute__((__section__(".timer_client_config"))) timer_client_config_t timer_config;
extern serial_client_config_t serial_config;
extern blk_client_config_t blk_config;
extern net_client_config_t net_config;
extern vmm_config_t vmm_config;

/* Data for the guest's kernel image. */
extern char _guest_kernel_image[];
extern char _guest_kernel_image_end[];
/* Data for the guest's ACPI Differentiated System Description Table (DSDT). */
extern char _guest_dsdt_aml[];
extern char _guest_dsdt_aml_end[];
/* Data for the initial RAM disk to be passed to the kernel. */
extern char _guest_initrd_image[];
extern char _guest_initrd_image_end[];

/* Bookkeeping structures for virtio devices */
extern struct virtio_console_device virtio_console;
extern struct virtio_blk_device virtio_blk;
extern struct virtio_net_device virtio_net;

/* sDDF data */
extern serial_queue_handle_t serial_rx_queue;
extern serial_queue_handle_t bridge_rx, bridge_tx;
extern struct virtio_vsock_device bridge_vsock;
extern serial_queue_handle_t serial_tx_queue;

extern blk_queue_handle_t blk_queue;

extern net_queue_handle_t net_rx_queue;
extern net_queue_handle_t net_tx_queue;

static linux_x86_setup_ret_t linux_setup;
static seL4_VCPUContext initial_regs;

#define GUEST_CMDLINE_HW "console=hvc0 quiet loglevel=1"
#define GUEST_CMDLINE_QEMU "console=hvc0 quiet loglevel=1 tsc=nowatchdog"
/* 768 MiB of RAM for the guest (GPA 0..0x30000000). In the VMM it is mapped at 4 GiB,
 * away from the regions sdfgen places from 0x30000000 up. Must match meta.py. */
#define GUEST_RAM_SIZE 0x30000000
#define GUEST_RAM_HVA 0x100000000

/* Matches DSDT (moved above the guest's RAM) */
#define PCI_MMIO_APERATURE_GPA 0xC0000000
#define PCI_MMIO_APERATURE_SIZE 0x80000

bool guest_arch_init(void)
{
    assert(timer_config_check_magic(&timer_config));

    arch_guest_init_t args = {
        .bsp = true,
        .timer_ch = timer_config.driver_id,
        .num_guest_ram_regions = 1,
        .guest_ram_regions = { (struct guest_ram_region) {
            .gpa_start = LOW_RAM_START_GPA, .size = GUEST_RAM_SIZE, .vmm_vaddr = (void *)GUEST_RAM_HVA },
        },
        .pci_init = (struct guest_pci_init) {
                                   .ecam_gpa = 0,
                                   .ecam_size = 0,
                                   .mmio_aperature_gpa = PCI_MMIO_APERATURE_GPA,
                                   .mmio_aperature_size = PCI_MMIO_APERATURE_SIZE,
                               },
    };

    if (!guest_init(args)) {
        LOG_VMM_ERR("Failed to initialise VMM\n");
        return false;
    }

    /* Place all the binaries in the right locations before starting the guest */
    size_t kernel_size = _guest_kernel_image_end - _guest_kernel_image;
    size_t initrd_size = _guest_initrd_image_end - _guest_initrd_image;
    size_t dsdt_aml_size = _guest_dsdt_aml_end - _guest_dsdt_aml;

    if (!kernel_size) {
        LOG_VMM_ERR("Kernel image is empty\n");
        return false;
    }
    if (!initrd_size) {
        LOG_VMM_ERR("Initial ramdisk image is empty\n");
        return false;
    }
    if (!dsdt_aml_size) {
        LOG_VMM_ERR("DSDT AML image is empty\n");
        return false;
    }

    char *cmdline;
    /* Reading a TSC does not cause a VM Exit, but the HPET does. On QEMU, the nested virtualisation overhead
     * is too high, leading to Linux's watchdog timer complaining:
     * [    3.568257] clocksource: Watchdog hpet read timed out. Readout sequence took: 57800ns
     * [    4.064535] clocksource: Watchdog hpet read timed out. Readout sequence took: 62000ns
     * [    9.064695] watchdog_print_freq_timeout: 7 callbacks suppressed
     * [    9.066802] clocksource: Watchdog hpet read timed out. Readout sequence took: 217500ns
     * [    9.568591] clocksource: Watchdog hpet read timed out. Readout sequence took: 72900ns
     *
     * So lets turn off the TSC watchdog if the VMM detects that it is running on QEMU. This is sound because
     * the virtual HPET time is derived from the TSC, and this problem doesn't manifest on hardware.
     */
    if (hypervisor_present()) {
        cmdline = GUEST_CMDLINE_QEMU;
    } else {
        cmdline = GUEST_CMDLINE_HW;
    }
    if (!linux_setup_images((uintptr_t)_guest_kernel_image, kernel_size, (uintptr_t)_guest_initrd_image, initrd_size,
                            _guest_dsdt_aml, dsdt_aml_size, cmdline, &initial_regs, &linux_setup)) {
        LOG_VMM_ERR("Failed to initialise guest images\n");
        return false;
    }

    return true;
}

bool virtio_arch_init(void)
{
    if (!virtio_pci_console_init(&virtio_console, 0, VIRTIO_CONSOLE_PCI_DEVICE_SLOT,
                                 X86_IOAPIC_IRQ_ROUTE(0, VIRTIO_CONSOLE_PCI_IOAPIC_PIN), &serial_rx_queue,
                                 &serial_tx_queue, serial_config.tx.id, serial_config.rx.id)) {
        LOG_VMM_ERR("Failed to initialise virtIO PCI Console device\n");
        return false;
    }

    if (!virtio_pci_vsock_init(&bridge_vsock, 0, BRIDGE_VSOCK_PCI_DEVICE_SLOT,
                               X86_IOAPIC_IRQ_ROUTE(0, BRIDGE_VSOCK_PCI_IOAPIC_PIN), &bridge_rx, &bridge_tx,
                               BRIDGE_CH)) {
        LOG_VMM_ERR("Failed to initialise the bridge's virtIO PCI vsock device\n");
        return false;
    }

#ifndef NO_VM_DISK                 /* live systems: no disk (Docker's data stays in the VM's RAM) */
    blk_storage_info_t *storage_info = blk_config.virt.storage_info.vaddr;
    if (!virtio_pci_blk_init(&virtio_blk, 0, VIRTIO_BLK_PCI_DEVICE_SLOT,
                             X86_IOAPIC_IRQ_ROUTE(0, VIRTIO_BLK_PCI_IOAPIC_PIN), (uintptr_t)blk_config.data.vaddr,
                             blk_config.data.size, storage_info, &blk_queue, blk_config.virt.num_buffers,
                             blk_config.virt.id)) {
        LOG_VMM_ERR("Failed to initialise virtIO PCI Block device\n");
        return false;
    }
#endif

    if (!virtio_pci_net_init(&virtio_net, 0, VIRTIO_NET_PCI_DEVICE_SLOT,
                             X86_IOAPIC_IRQ_ROUTE(0, VIRTIO_NET_PCI_IOAPIC_PIN), &net_rx_queue, &net_tx_queue,
                             (uintptr_t)net_config.rx_data.vaddr, (uintptr_t)net_config.tx_data.vaddr, net_config.rx.id,
                             net_config.tx.id, net_config.mac_addr.addr, HAVE_CSUM_OFFLOAD)) {
        LOG_VMM_ERR("Failed to initialise virtIO PCI Network device\n");
        return false;
    }

    return true;
}

bool guest_arch_start(void)
{
    return guest_start_long_mode(linux_setup.kernel_entry_gpa, linux_setup.pml4_gpa, linux_setup.gdt_gpa,
                                 linux_setup.gdt_limit, &initial_regs);
}