# GNU/seL4: native system (zsh, coreutils, FreeBSD networking...) + Linux VM with Docker, on seL4
#
#   make microkit-tool            (host)   Microkit tool with our patch (Rust in Docker)
#   make vm-rootfs                (host)   kernel + initramfs for the VM (busybox + Docker), with Docker
#   ../../dev.sh make             (Docker) everything: servers, drivers, VMM, sysroot, programs, ports
#                                          -> build/; installs /bin into the ext2 partition of disk.img
#   make test                     (host)   automated tests in QEMU (headless, on a copy of the disk)
#   make run                      (host)   boots in QEMU+KVM with a graphical console (quit: Ctrl-a, x
#                                          in this terminal); CONSOLE=serial: over the serial port
#   ../../dev.sh make disk-reset  deletes disk.img: it is recreated (empty) by the next make
#
# Console: Ctrl-\ 1 Enter -> VM (Docker); Ctrl-\ 0 Enter -> GNU/seL4
#
# SPDX-License-Identifier: BSD-2-Clause

# Dependencies (fetch-deps.sh): next to the repository, or elsewhere (the Docker image uses /deps)
export DEPS            ?= $(abspath ../..)
export MICROKIT_SDK    ?= $(DEPS)/sdk/microkit-sdk-2.3.1
export MICROKIT_BOARD  := x86_64_generic_vtx
# make RELEASE=1 (also run, test): kernel and system without debugging, in build-release/.
# Measured ~10 % faster on system-call-heavy work (fork, exec); it loses the components' debug
# messages (faults, mount, DHCP) on the serial port, so debug stays the default
ifneq ($(RELEASE),)
export MICROKIT_CONFIG := release
export BUILD_DIR       ?= $(abspath build-release)
endif
# make PLATFORM=latitude7390 [iso]: the live system for the real laptop, in its own directory
# (see system.mk); the ISO boots from GRUB (UEFI or BIOS, e.g. from a Ventoy USB stick) and runs
# entirely in RAM: no disk driver is included, so no disk is ever touched
export PLATFORM        ?= qemu
ifneq ($(PLATFORM),qemu)
export BUILD_DIR       ?= $(abspath build-$(PLATFORM))
endif
export MICROKIT_CONFIG ?= debug
# Our own Microkit tool (third_party/microkit, branch gnu-sel4: <cap_untyped>, <cap_cnode>...)
export MICROKIT_TOOL   := $(DEPS)/third_party/microkit/target/release/microkit
export LIBVMM          := $(DEPS)/libvmm
export SDDF            := $(LIBVMM)/dep/sddf
export POSIX           := $(abspath .)
# docker compose uses its own (build-docker/): its dependency paths differ (/deps)
export BUILD_DIR       ?= $(abspath build)

IMAGE := $(BUILD_DIR)/loader.img
# Host port forwards: host:8080 -> VM:80, host:2222 -> VM:22,
# host:8090 -> native GNU/seL4 (FreeBSD stack, 10.0.2.16):8080
HOSTFWD ?= hostfwd=tcp::8080-:80,hostfwd=tcp::2222-:22,hostfwd=tcp::8090-10.0.2.16:8080

all: $(BUILD_DIR)/Makefile
	$(MAKE) -C $(BUILD_DIR) all

$(BUILD_DIR)/Makefile: system.mk
	mkdir -p $(BUILD_DIR)
	cp system.mk $@

microkit-tool:
	microkit/build-tool.sh

vm-rootfs:
	docker run --rm -v "$(CURDIR)":/w -w /w/vm/rootfs alpine:3.24 \
		sh -c './build-rootfs.sh && chown -R $$(stat -c %u:%g .) out'

# q35 with the disk in slot 3 and the network in slot 4, and more than 4 GiB of RAM: the sDDF
# drivers expect the BARs and IRQs that the firmware assigns this way (see meta.py).
# Graphical console (default): a QEMU window with screen and keyboard; messages from the
# kernel and the components still go to this terminal (Ctrl-a x closes QEMU).
# make CONSOLE=serial: everything over the serial port (build with the same CONSOLE).
CONSOLE ?= graphical
ifeq ($(CONSOLE),serial)
DISPLAY_OPTS := -nographic
else
DISPLAY_OPTS := -display gtk -vga none -device bochs-display,addr=0x5
endif

run:
	@test -f $(IMAGE) || { echo "Missing $(IMAGE): build with ../../dev.sh make"; exit 1; }
	qemu-system-x86_64 -machine q35 -accel kvm \
		-cpu host,+fsgsbase,+pdpe1gb,+xsaveopt,+xsave,+vmx,+vme \
		-kernel $(BUILD_DIR)/sel4_32.elf -initrd $(IMAGE) -m 6G $(DISPLAY_OPTS) -serial mon:stdio \
		-drive file=disk.img,format=raw,if=none,id=hd -device virtio-blk-pci,drive=hd,addr=0x3.0 \
		-netdev user,id=net0,$(HOSTFWD) -device virtio-net-pci,netdev=net0,addr=0x4.0

# Automated tests in headless QEMU, on a copy of the disk (tools/test.py).
# make test NO_NET=1: skip the tests that need the internet; SLOW=1: add the expensive ones
test:
	@test -f $(IMAGE) || { echo "Missing $(IMAGE): build with ../../dev.sh make"; exit 1; }
	tools/test.py $(if $(NO_NET),--no-net) $(if $(SLOW),--slow)

# Grows the ext2 partition of an existing disk.img to EXT2_MB (system.mk), keeping everything
disk-grow:
	tools/disk-grow.py

# The live ISO (inside the development image: ../../dev.sh make PLATFORM=latitude7390 iso)
ISO := $(BUILD_DIR)/gnu-sel4-live-$(PLATFORM).iso
iso: all
	@test "$(PLATFORM)" != qemu || { echo "make iso needs PLATFORM=latitude7390"; exit 1; }
	rm -rf $(BUILD_DIR)/iso && mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp $(BUILD_DIR)/sel4_32.elf $(BUILD_DIR)/iso/boot/sel4.elf
	cp $(BUILD_DIR)/loader.img $(BUILD_DIR)/iso/boot/loader.img
	cp tools/grub-live.cfg $(BUILD_DIR)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(BUILD_DIR)/iso -- -volid GNU-SEL4-LIVE >/dev/null 2>&1
	@ls -l $(ISO)

disk-reset:
	rm -f disk.img $(BUILD_DIR)/install.stamp

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run test clean disk-grow disk-reset iso vm-rootfs microkit-tool
