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

export MICROKIT_SDK    ?= $(abspath ../../sdk/microkit-sdk-2.3.1)
export MICROKIT_BOARD  := x86_64_generic_vtx
export MICROKIT_CONFIG ?= debug
# Our own Microkit tool (third_party/microkit, branch gnu-sel4: <cap_untyped>, <cap_cnode>...)
export MICROKIT_TOOL   := $(abspath ../../third_party/microkit/target/release/microkit)
export LIBVMM          := $(abspath ../../libvmm)
export SDDF            := $(LIBVMM)/dep/sddf
export POSIX           := $(abspath .)
export BUILD_DIR       := $(abspath build)

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
# make test NO_NET=1: skip the tests that need the internet
test:
	@test -f $(IMAGE) || { echo "Missing $(IMAGE): build with ../../dev.sh make"; exit 1; }
	tools/test.py $(if $(NO_NET),--no-net)

disk-reset:
	rm -f disk.img build/install.stamp

clean:
	rm -rf build

.PHONY: all run test clean disk-reset vm-rootfs microkit-tool
