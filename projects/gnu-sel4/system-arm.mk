#
# The ARM build's own part of system.mk (included when ARCH=aarch64: make PLATFORM=qemu-arm).
# TODO item 16, milestone 2: QEMU's virt machine with the serial console (PL011), ARM's timer,
# fs_ext2 with the file system in RAM and posix_server; no disk, network or VM yet. The file
# system is rootfs/ and our own programs (apps/); the ports come with milestone 3.
#
# SPDX-License-Identifier: BSD-2-Clause

IMAGES := serial_driver.elf serial_virt_tx.elf serial_virt_rx.elf timer_driver.elf \
	  fs_ext2.elf posix_server.elf
$(IMAGES): libsddf_util_debug.a
include $(SDDF)/drivers/timer/$(TIMER_DRIV_DIR)/timer_driver.mk

# The machine's devices for sdfgen: $(DTB), from sDDF's description of QEMU's virt (its rule)

$(SYSTEM_FILE): $(POSIX)/meta.py $(IMAGES) $(DTB)
	PYTHONPATH=$(SDDF)/tools/meta:$$PYTHONPATH python3 $(POSIX)/meta.py --sddf $(SDDF) \
		--board $(MICROKIT_BOARD) --dtb $(DTB) --output . --sdf $@
	$(OBJCOPY) --update-section .device_resources=serial_driver_device_resources.data serial_driver.elf
	$(OBJCOPY) --update-section .serial_driver_config=serial_driver_config.data serial_driver.elf
	$(OBJCOPY) --update-section .serial_virt_rx_config=serial_virt_rx.data serial_virt_rx.elf
	$(OBJCOPY) --update-section .serial_virt_tx_config=serial_virt_tx.data serial_virt_tx.elf
	$(OBJCOPY) --update-section .serial_client_config=serial_client_posix_server.data posix_server.elf
	$(OBJCOPY) --update-section .timer_client_config=timer_client_posix_server.data posix_server.elf
	$(OBJCOPY) --update-section .device_resources=timer_driver_device_resources.data timer_driver.elf

# The file system in RAM: the installed partition, as the live system's (system.mk: PLATFORM
# qemu-arm sets LIVE; install.stamp, live-part1.img)
