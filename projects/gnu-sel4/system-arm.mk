#
# The ARM build's own part of system.mk (included when ARCH=aarch64: make PLATFORM=qemu-arm).
# TODO item 16: QEMU's virt machine with the serial console (PL011), ARM's timer, the disk
# (virtio-blk over MMIO, partition 1 for fs_ext2), posix_server; no network or VM yet.
#
# SPDX-License-Identifier: BSD-2-Clause

IMAGES := serial_driver.elf serial_virt_tx.elf serial_virt_rx.elf timer_driver.elf \
	  blk_driver.elf blk_virt.elf fs_ext2.elf posix_server.elf \
	  eth_driver.elf network_virt_rx.elf network_virt_tx.elf netstack.elf netstack_copy.elf
$(IMAGES): libsddf_util_debug.a
include $(SDDF)/drivers/timer/$(TIMER_DRIV_DIR)/timer_driver.mk

# The machine's devices for sdfgen: $(DTB), from sDDF's description of QEMU's virt (its rule)

$(SYSTEM_FILE): $(POSIX)/meta.py $(IMAGES) $(DTB)
	PYTHONPATH=$(SDDF)/tools/meta:$$PYTHONPATH python3 $(POSIX)/meta.py --sddf $(SDDF) \
		--board $(MICROKIT_BOARD) --dtb $(DTB) --output . --sdf $@
	# netstack's memory in 2 MiB pages (sdfgen has no option for it)
	sed -i -E 's|(<memory_region name="netstack_heap" size="0x[0-9a-f]+") />|\1 page_size="0x200000" />|' $@
	$(OBJCOPY) --update-section .device_resources=serial_driver_device_resources.data serial_driver.elf
	$(OBJCOPY) --update-section .serial_driver_config=serial_driver_config.data serial_driver.elf
	$(OBJCOPY) --update-section .serial_virt_rx_config=serial_virt_rx.data serial_virt_rx.elf
	$(OBJCOPY) --update-section .serial_virt_tx_config=serial_virt_tx.data serial_virt_tx.elf
	$(OBJCOPY) --update-section .serial_client_config=serial_client_posix_server.data posix_server.elf
	$(OBJCOPY) --update-section .timer_client_config=timer_client_posix_server.data posix_server.elf
	$(OBJCOPY) --update-section .device_resources=timer_driver_device_resources.data timer_driver.elf
	$(OBJCOPY) --update-section .device_resources=blk_driver_device_resources.data blk_driver.elf
	$(OBJCOPY) --update-section .blk_driver_config=blk_driver.data blk_driver.elf
	$(OBJCOPY) --update-section .blk_virt_config=blk_virt.data blk_virt.elf
	$(OBJCOPY) --update-section .blk_client_config=blk_client_fs_ext2.data fs_ext2.elf

# The file system: partition 1 of ARM's own disk (system.mk: DISK, install.stamp)
	$(OBJCOPY) --update-section .device_resources=eth_driver_device_resources.data eth_driver.elf
	$(OBJCOPY) --update-section .net_driver_config=net_driver.data eth_driver.elf
	$(OBJCOPY) --update-section .net_virt_rx_config=net_virt_rx.data network_virt_rx.elf
	$(OBJCOPY) --update-section .net_virt_tx_config=net_virt_tx.data network_virt_tx.elf
	$(OBJCOPY) --update-section .net_client_config=net_client_netstack.data netstack.elf
	$(OBJCOPY) --update-section .timer_client_config=timer_client_netstack.data netstack.elf
	$(OBJCOPY) --update-section .net_copy_config=net_copy_netstack_net_copier.data netstack_copy.elf
