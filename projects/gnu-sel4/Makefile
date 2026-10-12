# GNU/seL4: native system (zsh, coreutils, FreeBSD networking...) + Linux VM with Docker, on seL4
#
#   make microkit-tool            (host)   Microkit tool with our patch (Rust in Docker)
#   make vm-rootfs                (host)   kernel + initramfs for the VM (busybox + Docker), with Docker
#   make vm-lock                  (host)   pins the VM image to today's Alpine packages (deps.lock)
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
# make LINUX_VM=no (also run, test): no Linux VM, on a kernel without VT-x, for machines that
# cannot give seL4 VT-x (CI runners, AMD hosts); in build-novm/ (system.mk)
export LINUX_VM        ?= yes
ifeq ($(LINUX_VM),no)
export MICROKIT_BOARD  := x86_64_generic
QEMU_VMX               :=
else
export MICROKIT_BOARD  := x86_64_generic_vtx
QEMU_VMX               := ,+vmx
endif
# make RELEASE=1 (also run, test): kernel and system without debugging, in build-release/.
# Measured ~10 % faster on system-call-heavy work (fork, exec); it loses the components' debug
# messages (faults, mount, DHCP) on the serial port, so debug stays the default
ifneq ($(RELEASE),)
export MICROKIT_CONFIG := release
endif
# make PLATFORM=latitude7390 [iso]: the live system for the real laptop, in its own directory
# (see system.mk); the ISO boots from GRUB (UEFI or BIOS, e.g. from a Ventoy USB stick) and runs
# entirely in RAM: no disk driver is included, so no disk is ever touched
export PLATFORM        ?= qemu
# One build directory per variant: build[-PLATFORM][-release][-novm] (docker compose sets its own,
# build-docker/: its dependency paths differ, /deps)
export BUILD_DIR       ?= $(abspath build$(if $(filter-out qemu,$(PLATFORM)),-$(PLATFORM))$(if $(RELEASE),-release)$(if $(filter no,$(LINUX_VM)),-novm))
export MICROKIT_CONFIG ?= debug
# Our own Microkit tool (third_party/microkit, branch gnu-sel4: <cap_untyped>, <cap_cnode>...)
export MICROKIT_TOOL   := $(DEPS)/third_party/microkit/target/release/microkit
export LIBVMM          := $(DEPS)/libvmm
export SDDF            := $(LIBVMM)/dep/sddf
export POSIX           := $(abspath .)

IMAGE := $(BUILD_DIR)/loader.img
# Host port forwards: host:8080 -> VM:80, host:2222 -> VM:22,
# host:8090 -> native GNU/seL4 (FreeBSD stack, 10.0.2.16):8080, host:2223 -> its ssh (sshd service)
HOSTFWD ?= hostfwd=tcp::8080-:80,hostfwd=tcp::2222-:22,hostfwd=tcp::8090-10.0.2.16:8080,hostfwd=tcp::2223-10.0.2.16:22

all: $(BUILD_DIR)/Makefile
	$(MAKE) -C $(BUILD_DIR) all

$(BUILD_DIR)/Makefile: system.mk
	mkdir -p $(BUILD_DIR)
	cp system.mk $@

microkit-tool:
	microkit/build-tool.sh

# The VM image is pinned in deps.lock ("VM image" section): the base image by digest, every
# Alpine package and Docker by SHA-256 (fetch-deps.sh puts them in third_party/alpine-*);
# it is built with no network. make vm-lock moves the pins to today's packages.
VM_BASE = $(shell awk '$$1 == "image" && $$2 == "alpine" { print $$3 "@" $$4 }' $(DEPS)/deps.lock)
VM_PKGS = $(firstword $(wildcard $(DEPS)/third_party/alpine-*))
vm-rootfs:
	@test -n "$(VM_BASE)" -a -n "$(VM_PKGS)" || { echo "VM image not fetched: run ../../fetch-deps.sh"; exit 1; }
	docker run --rm --network none -v "$(CURDIR)":/w -v "$(VM_PKGS)":/pkgs:ro \
		-v "$(DEPS)/deps.lock":/deps.lock:ro -w /w/vm/rootfs \
		$(VM_BASE) sh -c './build-rootfs.sh && chown -R $$(stat -c %u:%g .) out'

vm-lock:
	vm/rootfs/lock.sh

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
		-cpu host,host-phys-bits=off,phys-bits=39,+fsgsbase,+pdpe1gb,+xsaveopt,+xsave$(QEMU_VMX),+vme,+invtsc \
		-kernel $(BUILD_DIR)/sel4_32.elf -initrd $(IMAGE) -m 6G $(DISPLAY_OPTS) -serial mon:stdio \
		-drive file=disk.img,format=raw,if=none,id=hd -device virtio-blk-pci,drive=hd,addr=0x3.0 \
		-netdev user,id=net0,$(HOSTFWD) -device virtio-net-pci,netdev=net0,addr=0x4.0

# Automated tests in headless QEMU, on a copy of the disk (tools/test.py).
# make test NO_NET=1: skip the tests that need the internet; SLOW=1: add the expensive ones;
# ONLY=a,b: just those (quick, while working on one thing)
test:
	@test -f $(IMAGE) || { echo "Missing $(IMAGE): build with ../../dev.sh make"; exit 1; }
	tools/test.py $(if $(NO_NET),--no-net) $(if $(SLOW),--slow) $(if $(ONLY),--only $(ONLY))

# Grows the ext2 partition of an existing disk.img to EXT2_MB (system.mk), keeping everything
disk-grow:
	tools/disk-grow.py

# The live ISO (inside the development image: ../../dev.sh make PLATFORM=latitude7390 iso).
# Every ISO gets the next build number (in its name, the GRUB menu and the test log);
# make ventoy (on the host) copies the last one to the mounted Ventoy USB stick
LIVE_COUNTER := $(BUILD_DIR)/live-build-number
LAST_BUILD := $(shell cat $(LIVE_COUNTER) 2>/dev/null || echo 0)
export LIVE_BUILD := $(shell printf %03d $$(( $(LAST_BUILD) + 1 )))
ISO = $(BUILD_DIR)/gnu-sel4-live-$(PLATFORM)-build$(1).iso
iso: all
	@test "$(PLATFORM)" != qemu || { echo "make iso needs PLATFORM=latitude7390"; exit 1; }
	rm -rf $(BUILD_DIR)/iso && mkdir -p $(BUILD_DIR)/iso/boot/grub
	cp $(BUILD_DIR)/sel4_32.elf $(BUILD_DIR)/iso/boot/sel4.elf
	cp $(BUILD_DIR)/loader.img $(BUILD_DIR)/iso/boot/loader.img
	sed 's/@BUILD@/$(LIVE_BUILD)/g' tools/grub-live.cfg > $(BUILD_DIR)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $(call ISO,$(LIVE_BUILD)) $(BUILD_DIR)/iso -- -volid GNU-SEL4-$(LIVE_BUILD) >/dev/null 2>&1
	echo $(LIVE_BUILD) > $(LIVE_COUNTER)
	@ls -l $(call ISO,$(LIVE_BUILD))

# Copies the last live ISO to the Ventoy stick: only to a mounted file system labelled Ventoy on a
# USB device, as a plain file copy (nothing else on the stick or any other disk is touched)
VENTOY ?= $(shell findmnt -rn -S LABEL=Ventoy -o TARGET 2>/dev/null)
ventoy:
	@test -n "$(VENTOY)" || { echo "no mounted Ventoy stick (a file system labelled Ventoy)"; exit 1; }
	@src=$$(findmnt -rn -o SOURCE "$(VENTOY)"); disk=$$(lsblk -no PKNAME "$$src"); \
	 [ "$$(lsblk -dno TRAN /dev/$$disk)" = usb ] || { echo "$(VENTOY) is not on a USB device: not copying"; exit 1; }
	@f=$(call ISO,$$(printf %03d $(LAST_BUILD))); test -f "$$f" || { echo "no ISO $$f: make iso first"; exit 1; }; \
	 cp "$$f" "$(VENTOY)/" && sync -f "$(VENTOY)/$$(basename $$f)" && ls -l "$(VENTOY)/$$(basename $$f)"

disk-reset:

disk-reset:
	rm -f disk.img $(BUILD_DIR)/install.stamp

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all run test clean disk-grow disk-reset iso ventoy vm-rootfs vm-lock microkit-tool
