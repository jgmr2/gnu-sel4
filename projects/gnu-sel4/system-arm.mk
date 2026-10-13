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

# --- the file system in RAM (inside fs_ext2: system.mk's fs_ext2_ramdisk.o) ---
# rootfs/ plus our programs, /bin or /sbin as on x86 (SBIN_APPS); everything root's but the
# user's home (mke2fs -d keeps the build host's owners: debugfs sets them)
ARM_RAMFS_MB := 64
ARM_SBIN := init 9pd poweroff
ARM_BINS := $(addprefix bin/,$(notdir $(wildcard $(POSIX)/apps/*)))
live-part1.img: $(ARM_BINS) $(shell find $(POSIX)/rootfs -type f)
	rm -rf ramfs && mkdir -p ramfs/bin ramfs/sbin ramfs/mnt ramfs/run ramfs/tmp
	cp -a $(POSIX)/rootfs/. ramfs/
	for b in $(ARM_BINS); do n=$$(basename $$b); \
	  case " $(ARM_SBIN) " in *" $$n "*) cp $$b ramfs/sbin/;; *) cp $$b ramfs/bin/;; esac; done
	ln -sf poweroff ramfs/sbin/reboot && ln -sf poweroff ramfs/sbin/halt
	rm -f $@.tmp && truncate -s $(ARM_RAMFS_MB)M $@.tmp
	mke2fs -q -F -t ext2 -b 4096 -I 256 -L gnu-sel4 -O ^dir_index,^resize_inode -E root_owner=0:0 -d ramfs $@.tmp
	(cd ramfs && find . -mindepth 1 | sed 's|^\.||' | while read -r p; do \
	   echo "sif $$p uid 0"; echo "sif $$p gid 0"; done; \
	 echo "sif /tmp mode 041777"; echo "sif /root mode 040700"; \
	 [ -d home/user ] && { echo "sif /home/user uid 1000"; echo "sif /home/user gid 1000"; }; true) > ramfs.cmds
	debugfs -w -f ramfs.cmds $@.tmp >/dev/null 2>ramfs.log || { cat ramfs.log; exit 1; }
	mv $@.tmp $@
