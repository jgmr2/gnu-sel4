#
# GNU/seL4 build rules (copied to build/Makefile and run there, in Docker).
# Input variables: MICROKIT_SDK MICROKIT_BOARD MICROKIT_CONFIG SDDF LIBVMM POSIX BUILD_DIR DEPS
#
# SPDX-License-Identifier: BSD-2-Clause
SUPPORTED_BOARDS := x86_64_generic_vtx
# Third-party sources (fetch-deps.sh puts them in $(DEPS)/third_party)
TP := $(DEPS)/third_party
TOOLCHAIN ?= clang
SDDF_CUSTOM_LIBC := 1
include $(SDDF)/tools/make/board/common.mk

SYSTEM_FILE := gnu-sel4.system
IMAGE_FILE  := loader.img
REPORT_FILE := report.txt

# --- sDDF components: console (COM1), disk and network (virtio), timer, with their virtualisers ---
# The virtio drivers do not read PCI configuration space: the q35 BARs are fixed (see meta.py)
SERIAL_COMPONENTS := $(SDDF)/serial/components
BLK_COMPONENTS := $(SDDF)/blk/components
NET_COMPONENTS := $(SDDF)/network/components
CFLAGS += -I$(SDDF)/include -I$(SDDF)/include/microkit -I$(LIBVMM)/include -Wno-unused-function \
	  -DBOARD_$(MICROKIT_BOARD) -DSDDF_VIRTIO_PCI_TRANSPORT_SKIP_BUS_CHECK
LIBS := --start-group -lmicrokit -Tmicrokit.ld libsddf_util_debug.a --end-group
include $(SDDF)/util/util.mk
include $(SDDF)/drivers/serial/$(UART_DRIV_DIR)/serial_driver.mk
include $(SERIAL_COMPONENTS)/serial_components.mk
include $(SDDF)/drivers/blk/$(BLK_DRIV_DIR)/blk_driver.mk
include $(BLK_COMPONENTS)/blk_components.mk
include $(SDDF)/drivers/network/$(NET_DRIV_DIR)/eth_driver.mk
include $(NET_COMPONENTS)/network_components.mk
include $(SDDF)/drivers/timer/$(TIMER_DRIV_DIR)/timer_driver.mk
include $(LIBVMM)/vmm.mk
vpath %.c $(SDDF) $(LIBVMM)

IMAGES := serial_driver.elf serial_virt_tx.elf serial_virt_rx.elf blk_driver.elf blk_virt.elf \
	  eth_driver.elf network_virt_rx.elf network_virt_tx.elf network_copy.elf timer_driver.elf \
	  fs_ext2.elf posix_server.elf client_vmm.elf netstack.elf netstack_copy.elf fbcon.elf
$(IMAGES): libsddf_util_debug.a

# --- syscall tables generated from musl (Linux x86_64 numbers and names) ---
MUSL_SRC := $(TP)/musl-1.2.6
syscall_nr.h: $(MUSL_SRC)/arch/x86_64/bits/syscall.h.in
	cp $< $@
syscall_names.h: $(MUSL_SRC)/arch/x86_64/bits/syscall.h.in
	{ echo 'static const char *const syscall_names[] = {'; \
	  sed -n 's/^#define __NR_\([a-z0-9_]*\)[[:space:]]*\([0-9]*\)$$/  [\2] = "\1",/p' $<; \
	  echo '};'; } > $@

# --- graphical console: screen + PS/2 keyboard as the sDDF "serial driver" (console/) ---
SPLEEN := $(TP)/spleen/spleen-8x16.bdf
font_spleen.h: $(SPLEEN) $(POSIX)/tools/bdf2c.py
	python3 $(POSIX)/tools/bdf2c.py $< $@
console_%.o: $(POSIX)/console/%.c $(POSIX)/console/term.h font_spleen.h
	$(CC) $(CFLAGS) -I. -c $< -o $@
fbcon.elf: console_fbcon.o console_term.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# --- POSIX server: dispatch + processes + terminal + VFS ---
SERVER_SRCS := posix_server proc tty vfs clock bridge mem procfs cred
SERVER_DEPS := $(wildcard $(POSIX)/server/*.h) $(wildcard $(POSIX)/include/sel4posix/*.h) \
	       syscall_nr.h syscall_names.h
server_%.o: $(POSIX)/server/%.c $(SERVER_DEPS)
	$(CC) $(CFLAGS) -I$(POSIX)/include -I. -c $< -o $@
posix_server.elf: $(addprefix server_,$(addsuffix .o,$(SERVER_SRCS)))
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# --- ext2 file server ---
fs_ext2.o: $(POSIX)/fs/ext2/fs_ext2.c $(POSIX)/include/sel4posix/fs_protocol.h
	$(CC) $(CFLAGS) -I$(POSIX)/include -DBUILD_TIME=$(shell date +%s) -c $< -o $@
fs_ext2.elf: fs_ext2.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# --- virtual machine VMM (libvmm): minimal Linux VM with Docker ---
VM := $(POSIX)/vm
VM_KERNEL := $(VM)/rootfs/out/vmlinuz
VM_INITRD := $(VM)/rootfs/out/initramfs.cpio.gz
$(VM_KERNEL) $(VM_INITRD):
	@echo "Missing the VM rootfs: run 'make vm-rootfs' on the host"; exit 1
vm_dsdt.aml: $(VM)/virtio_pci_dsdt.dsl
	iasl -p $@ $< >/dev/null
vm_%.o: $(VM)/%.c $(wildcard $(VM)/*.h)
	$(CC) $(CFLAGS) -I$(VM) -c $< -o $@
vm_images.o: $(LIBVMM)/tools/package_guest_images.S $(VM_KERNEL) $(VM_INITRD) vm_dsdt.aml
	$(CC) -c -g3 -x assembler-with-cpp -DGUEST_KERNEL_IMAGE_PATH=\"$(VM_KERNEL)\" \
		-DGUEST_DSDT_AML_PATH=\"vm_dsdt.aml\" -DGUEST_INITRD_IMAGE_PATH=\"$(VM_INITRD)\" \
		-target $(TARGET) $< -o $@
client_vmm.elf: vm_client_vmm.o vm_guest_arch_init.o vm_images.o libvmm.a
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# --- sysroot for programs: ported musl + runtime + sel4-cc ---
SYSROOT := $(BUILD_DIR)/sysroot
LIBC    := $(SYSROOT)/lib/libc.a
SEL4CC  := $(SYSROOT)/bin/sel4-cc
$(LIBC): $(POSIX)/musl/port-musl.sh
	$(POSIX)/musl/port-musl.sh $(MUSL_SRC) /tmp/musl-sel4 $(SYSROOT)

RT_CFLAGS := --target=x86_64-linux-musl -O2 -g -Wall -fno-pic -fno-stack-protector -mfsgsbase \
	     -nostdinc -isystem $(SYSROOT)/include -I$(BOARD_DIR)/include -I$(POSIX)/include
RT_DEPS := $(wildcard $(POSIX)/include/sel4posix/*.h) $(LIBC)
rt_%.o: $(POSIX)/runtime/%.c $(RT_DEPS)
	clang $(RT_CFLAGS) -c $< -o $@
rt_crt0.o: $(POSIX)/runtime/crt0.S
	clang --target=x86_64-linux-musl -c $< -o $@
$(SYSROOT)/lib/crt0.o: rt_crt0.o
	cp $< $@
$(SYSROOT)/lib/libsel4posix.a: rt_start.o rt_syscall.o
	rm -f $@ && llvm-ar rcs $@ $^
$(SEL4CC): $(POSIX)/tools/sel4-cc
	mkdir -p $(dir $@) && cp $< $@
SYSROOT_READY := $(LIBC) $(SYSROOT)/lib/crt0.o $(SYSROOT)/lib/libsel4posix.a $(SEL4CC)
sysroot: $(SYSROOT_READY)

# --- netstack: the FreeBSD TCP/IP stack (F-Stack) as a component, with musl as its libc ---
FSTACK := $(TP)/f-stack
fstack/fstack.o: $(POSIX)/ports/fstack/build-fstack.sh
	FSTACK=$(FSTACK) $< fstack > fstack.log 2>&1 || { tail -20 fstack.log; exit 1; }
NET_CFLAGS := --target=x86_64-none-elf -O2 -g -Wall -fno-pic -fno-stack-protector -mfsgsbase -mrdrnd \
	      -nostdinc -isystem $(SYSROOT)/include -I$(BOARD_DIR)/include -I$(SDDF)/include \
	      -I$(SDDF)/include/microkit -I$(FSTACK)/lib -I$(POSIX)/include -I$(POSIX)/net/compat -D_GNU_SOURCE -D__thread= -include fstack_host.h -DINET -DINET6
# Address netstack asks for first over DHCP (QEMU: the target of the `make run` port
# forwards); empty = whatever the server hands out
NETSTACK_PREFERRED_IP ?= 10.0.2.16
NET_CFLAGS += $(if $(NETSTACK_PREFERRED_IP),-DNETSTACK_PREFERRED_IP='"$(NETSTACK_PREFERRED_IP)"')
netstack-ip.$(NETSTACK_PREFERRED_IP):              # changing it rebuilds netstack
	rm -f netstack-ip.* && touch '$@'
net_%.o: $(POSIX)/net/%.c $(POSIX)/net/netstack.h $(POSIX)/net/dhcp.h $(LIBC) netstack-ip.$(NETSTACK_PREFERRED_IP)
	clang $(NET_CFLAGS) -c $< -o $@
net_tsc.o: $(SDDF)/util/arch_timestamp_counter.c $(LIBC)
	clang $(NET_CFLAGS) -c $< -o $@
netstack.elf: net_netstack.o net_host.o net_dhcp.o net_tsc.o fstack/fstack.o $(LIBC)
	$(LD) $(LDFLAGS) net_netstack.o net_host.o net_dhcp.o net_tsc.o fstack/fstack.o \
		--start-group -lmicrokit -Tmicrokit.ld $(LIBC) --end-group -o $@
# second sDDF network copier (each one carries its configuration inside the ELF)
netstack_copy.elf: network_copy.elf
	cp $< $@

# --- programs from apps/<name>/*.c -> bin/<name> (in /bin, or in /sbin for SBIN_APPS) ---
SBIN_APPS := init 9pd poweroff
SBIN_LINKS := reboot:poweroff halt:poweroff
APPS := $(notdir $(wildcard $(POSIX)/apps/*))
BINS := $(addprefix bin/,$(APPS))
.SECONDEXPANSION:
bin/%: $(SYSROOT_READY) $$(wildcard $(POSIX)/apps/%/*.c)
	mkdir -p bin
	$(SEL4CC) -O2 -g -Wall -I$(POSIX)/include -o $@ $(wildcard $(POSIX)/apps/$*/*.c)

# --- ported programs ---
# They all depend on $(SYSROOT_READY): if the runtime (libsel4posix.a) or musl changes, they
# are rebuilt and relinked (otherwise they would keep the old runtime inside).
DASH_SRC := $(TP)/dash-0.5.13.5
dash/src/dash: $(SYSROOT_READY) $(POSIX)/ports/dash/build-dash.sh $(POSIX)/ports/dash/mksignames.c
	$(POSIX)/ports/dash/build-dash.sh $(DASH_SRC) dash $(SYSROOT)

# GNU coreutils: a multi-call binary; its programs are symlinked at install time
CU_SRC := $(TP)/coreutils-9.12
coreutils/src/coreutils: $(SYSROOT_READY) $(POSIX)/ports/coreutils/build-coreutils.sh
	$(POSIX)/ports/coreutils/build-coreutils.sh $(CU_SRC) coreutils $(SYSROOT)
coreutils.bin: coreutils/src/coreutils
	llvm-strip -o $@ $<

# ncurses: library in the sysroot (used by zsh and vim) + its programs (clear, tput, reset...)
NC_SRC := $(TP)/ncurses-6.6
NCURSES := ncurses/.built
$(NCURSES): $(SYSROOT_READY) $(POSIX)/ports/ncurses/build-ncurses.sh
	$(POSIX)/ports/ncurses/build-ncurses.sh $(NC_SRC) ncurses $(SYSROOT)
	touch $@
NC_PROGS := clear tput reset tset infocmp tic toe tabs
ncurses/inst/usr/bin/%: $(NCURSES)
	@test -f $@

# zsh (static, modules built into the binary)
ZSH_SRC := $(TP)/zsh-5.9.2
zsh/Src/zsh: $(NCURSES) $(SYSROOT_READY) $(POSIX)/ports/zsh/build-zsh.sh
	$(POSIX)/ports/zsh/build-zsh.sh $(ZSH_SRC) zsh $(SYSROOT)

# vim (text mode) with a trimmed runtime
VIM_SRC := $(TP)/vim-9.2.1172
vim/src/vim: $(NCURSES) $(SYSROOT_READY) $(POSIX)/ports/vim/build-vim.sh
	$(POSIX)/ports/vim/build-vim.sh $(VIM_SRC) vim $(SYSROOT)
vim.bin: vim/src/vim
	llvm-strip -o $@ $<

# procps-ng: ps, top, free... read posix_server's /proc (server/procfs.c)
PROCPS_SRC := $(TP)/procps-ng-4.0.7
PROCPS_PROGS := ps top free uptime pgrep pkill pidof watch vmstat pwdx tload
procps/.built: $(NCURSES) $(SYSROOT_READY) $(POSIX)/ports/procps/build-procps.sh
	$(POSIX)/ports/procps/build-procps.sh $(PROCPS_SRC) procps $(SYSROOT)
	mkdir -p procps/bin && for p in $(PROCPS_PROGS); do llvm-strip -o procps/bin/$$p procps/inst/usr/local/bin/$$p; done
	touch $@
$(addprefix procps/bin/,$(PROCPS_PROGS)): procps/.built ;

# curl with HTTPS (mbedTLS in the sysroot) and Mozilla's root certificates
MBEDTLS_SRC := $(TP)/mbedtls-3.6.7
CURL_SRC := $(TP)/curl-8.22.0
CACERT := $(TP)/cacert/cacert.pem
curl/curl/src/curl: $(SYSROOT_READY) $(POSIX)/ports/curl/build-curl.sh
	$(POSIX)/ports/curl/build-curl.sh $(MBEDTLS_SRC) $(CURL_SRC) $(CACERT) curl $(SYSROOT)
curl.bin: curl/curl/src/curl
	llvm-strip -o $@ $<

# socat (and /bin/nc, a script on top of it) and the dropbear ssh client (ssh, scp, dropbearkey)
SOCAT_SRC := $(TP)/socat-1.8.1.3
socat/socat: $(SYSROOT_READY) $(POSIX)/ports/socat/build-socat.sh
	$(POSIX)/ports/socat/build-socat.sh $(SOCAT_SRC) socat $(SYSROOT)
socat.bin: socat/socat
	llvm-strip -o $@ $<
DROPBEAR_SRC := $(TP)/dropbear-2026.94
dropbear/dropbearmulti: $(SYSROOT_READY) $(POSIX)/ports/dropbear/build-dropbear.sh
	$(POSIX)/ports/dropbear/build-dropbear.sh $(DROPBEAR_SRC) dropbear $(SYSROOT)
dropbear.bin: dropbear/dropbearmulti
	llvm-strip -o $@ $<

# Everyday GNU tools and less (ports/gnu/build-gnu.sh): sources and "package/program"
GNU_PKGS := grep-3.12 sed-4.10 findutils-4.11.0 diffutils-3.12 tar-1.35 gzip-1.15 less-710
GNU_PROGS := grep/grep grep/egrep grep/fgrep sed/sed findutils/find findutils/xargs \
	     diffutils/cmp diffutils/diff diffutils/diff3 diffutils/sdiff tar/tar \
	     gzip/gzip gzip/gunzip gzip/zcat gzip/zgrep gzip/zless gzip/zmore gzip/zdiff gzip/zcmp less/less
GNU_ARGS_diffutils := gl_cv_func_strcasecmp_works=yes
GNU_ARGS_less := --with-regex=posix LIBS=-lncursesw
gnu-%/.built: $(SYSROOT_READY) $(NCURSES) $(POSIX)/ports/gnu/build-gnu.sh
	$(POSIX)/ports/gnu/build-gnu.sh $(TP)/$(filter $*-%,$(GNU_PKGS)) gnu-$* $(SYSROOT) $(GNU_ARGS_$*)
	touch $@
$(foreach n,$(sort $(dir $(GNU_PROGS))),$(eval gnu-$(n)bin/%: gnu-$(n).built ; @test -f $$@))
GNU_BINS := $(foreach p,$(GNU_PROGS),gnu-$(dir $(p))bin/$(notdir $(p)):$(notdir $(p)))

EXTRA_BINS := dash/src/dash:dash coreutils.bin:coreutils zsh/Src/zsh:zsh vim.bin:vim curl.bin:curl \
	      socat.bin:socat dropbear.bin:dropbearmulti $(GNU_BINS) \
	      $(foreach p,$(NC_PROGS),ncurses/inst/usr/bin/$(p):$(p)) \
	      $(foreach p,$(PROCPS_PROGS),procps/bin/$(p):$(p))
# skarnet.org: skalibs (library) -> execline -> s6 -> s6-rc (ports/skarnet/build-skarnet.sh).
# Their programs go to /bin; execline is one multicall binary plus symlinks (links.txt)
SKARNET_PKGS := skalibs-2.15.1.0 execline-2.9.9.2 s6-2.15.1.0 s6-rc-0.7.0.0
sk_dir = $(TP)/$(firstword $(filter $(1)-%,$(SKARNET_PKGS)))   # s6-% also matches s6-rc
sk-skalibs/.built: $(SYSROOT_READY) $(POSIX)/ports/skarnet/build-skarnet.sh
sk-execline/.built: sk-skalibs/.built
sk-s6/.built: sk-execline/.built
sk-s6-rc/.built: sk-s6/.built
sk-%/.built:
	$(POSIX)/ports/skarnet/build-skarnet.sh $(call sk_dir,$*) sk-$* $(SYSROOT)
	touch $@
SKARNET_BUILT := sk-execline/.built sk-s6/.built sk-s6-rc/.built

# License texts of everything on the disk -> /usr/share/licenses/<project>/ (see THIRD_PARTY.md)
gnu_dir = $(firstword $(subst -, ,$(1)))
LICENSE_FILES := gnu-sel4:$(POSIX)/../../LICENSE gnu-sel4:$(POSIX)/../../THIRD_PARTY.md \
	sel4:$(MICROKIT_SDK)/LICENSES/GPL-2.0-only.txt sel4:$(MICROKIT_SDK)/LICENSES/BSD-2-Clause.txt \
	microkit:$(MICROKIT_SDK)/LICENSES/BSD-2-Clause.txt sddf:$(SDDF)/LICENSES/BSD-2-Clause.txt \
	libvmm:$(LIBVMM)/LICENSES/BSD-2-Clause.txt musl:$(TP)/musl-1.2.6/COPYRIGHT \
	f-stack:$(TP)/f-stack/LICENSE dash:$(TP)/dash-0.5.13.5/COPYING zsh:$(TP)/zsh-5.9.2/LICENCE \
	vim:$(TP)/vim-9.2.1172/LICENSE ncurses:$(TP)/ncurses-6.6/COPYING \
	coreutils:$(TP)/coreutils-9.12/COPYING $(foreach p,$(GNU_PKGS),$(call gnu_dir,$(p)):$(TP)/$(p)/COPYING) \
	less:$(TP)/less-710/LICENSE procps-ng:$(TP)/procps-ng-4.0.7/COPYING procps-ng:$(TP)/procps-ng-4.0.7/COPYING.LIB \
	curl:$(TP)/curl-8.22.0/COPYING mbedtls:$(TP)/mbedtls-3.6.7/LICENSE \
	socat:$(TP)/socat-1.8.1.3/COPYING socat:$(TP)/socat-1.8.1.3/COPYING.OpenSSL \
	dropbear:$(TP)/dropbear-2026.94/LICENSE spleen:$(TP)/spleen/LICENSE \
	$(foreach p,$(SKARNET_PKGS),$(call gnu_dir,$(p)):$(TP)/$(p)/COPYING) \
	llvm-compiler-rt:$(SDDF)/LICENSES/Apache-2.0.txt llvm-compiler-rt:$(SDDF)/LICENSES/LLVM-exception.txt
licenses/.built: $(foreach l,$(LICENSE_FILES),$(lastword $(subst :, ,$(l))))
	rm -rf licenses && mkdir licenses
	for l in $(LICENSE_FILES); do d=licenses/$${l%%:*}; mkdir -p $$d; cp $${l#*:} $$d/; done
	mkdir -p licenses/freebsd licenses/ca-certificates
	sed -n '1,/\*\//p' $(TP)/f-stack/freebsd/sys/queue.h > licenses/freebsd/BSD-3-Clause
	echo "Mozilla CA certificates (from https://curl.se/ca/cacert.pem): MPL-2.0, https://mozilla.org/MPL/2.0/" \
		> licenses/ca-certificates/LICENSE
	touch $@
# Data trees copied to the disk: "source:destination"
# s6-rc service sources (rootfs/etc/s6-rc), compiled at boot by /etc/rc
DATA_TREES := $(POSIX)/rootfs/etc/s6-rc:/etc/s6-rc zsh/inst/usr/share/zsh:/usr/share/zsh vim/runtime-min:/usr/share/vim/vim92 curl/ssl:/etc/ssl/certs \
	      licenses:/usr/share/licenses
# Symlinks: "name:target". The coreutils ones come from coreutils/programs.txt
# Linux VM commands available as native ones (bridge, like WSL interop)
LINUX_CMDS := docker
LINKS := sh:dash vi:vim $(foreach c,$(LINUX_CMDS),$(c):linux) \
	 ssh:dropbearmulti dbclient:dropbearmulti scp:dropbearmulti dropbearkey:dropbearmulti
# terminfo: only the terminals in use (the full database has ~1900)
TERMINFO := x/xterm x/xterm-256color x/xterm-color v/vt100 v/vt102 v/vt220 l/linux s/screen s/screen-256color d/dumb a/ansi

# --- system description + sDDF configuration patched into each ELF ---
# Switching consoles (make CONSOLE=serial|graphical) regenerates the system description
CONSOLE ?= graphical
export CONSOLE
console.$(CONSOLE):
	rm -f console.* && touch $@
$(SYSTEM_FILE): $(POSIX)/meta.py $(IMAGES) console.$(CONSOLE)
	PYTHONPATH=$(SDDF)/tools/meta:$$PYTHONPATH python3 $(POSIX)/meta.py --sddf $(SDDF) \
		--board $(MICROKIT_BOARD) --output . --sdf $@
	# large regions (VM RAM, netstack memory) in 2 MiB pages (sdfgen has no option for it)
	sed -i -E 's|(<memory_region name="(guest_ram\|netstack_heap)" size="0x[0-9a-f]+") />|\1 page_size="0x200000" />|' $@
	grep -q 'name="guest_ram".*page_size' $@
	if [ "$(CONSOLE)" = serial ]; then \
	  $(OBJCOPY) --update-section .device_resources=serial_driver_device_resources.data serial_driver.elf && \
	  $(OBJCOPY) --update-section .serial_driver_config=serial_driver_config.data serial_driver.elf; \
	else \
	  $(OBJCOPY) --update-section .serial_driver_config=serial_driver_config.data fbcon.elf; \
	fi
	$(OBJCOPY) --update-section .serial_virt_rx_config=serial_virt_rx.data serial_virt_rx.elf
	$(OBJCOPY) --update-section .serial_virt_tx_config=serial_virt_tx.data serial_virt_tx.elf
	$(OBJCOPY) --update-section .serial_client_config=serial_client_posix_server.data posix_server.elf
	$(OBJCOPY) --update-section .timer_client_config=timer_client_posix_server.data posix_server.elf
	$(OBJCOPY) --update-section .device_resources=blk_driver_device_resources.data blk_driver.elf
	$(OBJCOPY) --update-section .blk_driver_config=blk_driver.data blk_driver.elf
	$(OBJCOPY) --update-section .blk_virt_config=blk_virt.data blk_virt.elf
	$(OBJCOPY) --update-section .blk_client_config=blk_client_fs_ext2.data fs_ext2.elf
	$(OBJCOPY) --update-section .device_resources=eth_driver_device_resources.data eth_driver.elf
	$(OBJCOPY) --update-section .net_driver_config=net_driver.data eth_driver.elf
	$(OBJCOPY) --update-section .net_virt_rx_config=net_virt_rx.data network_virt_rx.elf
	$(OBJCOPY) --update-section .net_virt_tx_config=net_virt_tx.data network_virt_tx.elf
	$(OBJCOPY) --update-section .net_copy_config=net_copy_vm_net_copier.data network_copy.elf
	$(OBJCOPY) --update-section .device_resources=timer_driver_device_resources.data timer_driver.elf
	$(OBJCOPY) --update-section .timer_client_config=timer_client_CLIENT_VMM.data client_vmm.elf
	$(OBJCOPY) --update-section .serial_client_config=serial_client_CLIENT_VMM.data client_vmm.elf
	$(OBJCOPY) --update-section .blk_client_config=blk_client_CLIENT_VMM.data client_vmm.elf
	$(OBJCOPY) --update-section .net_client_config=net_client_CLIENT_VMM.data client_vmm.elf
	$(OBJCOPY) --update-section .vmm_config=vmm_CLIENT_VMM.data client_vmm.elf
	$(OBJCOPY) --update-section .net_client_config=net_client_netstack.data netstack.elf
	$(OBJCOPY) --update-section .timer_client_config=timer_client_netstack.data netstack.elf
	$(OBJCOPY) --update-section .net_copy_config=net_copy_netstack_net_copier.data netstack_copy.elf

$(IMAGE_FILE) $(REPORT_FILE): $(IMAGES) $(SYSTEM_FILE)
	$(MICROKIT_TOOL) $(SYSTEM_FILE) --search-path . --board $(MICROKIT_BOARD) \
		--config $(MICROKIT_CONFIG) -o $(IMAGE_FILE) -r $(REPORT_FILE)
	cp $(BOARD_DIR)/elf/sel4_32.elf .

# --- disk: MBR with two partitions ---
#   1: GNU/seL4 ext2 (initial contents: rootfs/)         -> fs_ext2
#   2: the VM's /var/lib/docker (the VM formats it ext4)  -> CLIENT_VMM
# Created only once (persistent). To recreate it: make disk-reset
DISK := $(POSIX)/disk.img
EXT2_MB := 64
DOCKER_GB := 8
$(DISK):
	set -e; \
	truncate -s $(EXT2_MB)M part.img; \
	mke2fs -q -F -t ext2 -b 4096 -I 256 -L gnu-sel4 -O ^dir_index,^resize_inode \
		-E root_owner=0:0 -d $(POSIX)/rootfs part.img; \
	truncate -s $$(( 1 + $(EXT2_MB) ))M $@.tmp; truncate -s +$(DOCKER_GB)G $@.tmp; \
	printf 'label: dos\nstart=2048, size=%sMiB, type=83\ntype=83\n' $(EXT2_MB) | sfdisk -q $@.tmp; \
	dd if=part.img of=$@.tmp bs=1M seek=1 conv=notrunc status=none; \
	rm part.img; mv $@.tmp $@; \
	echo "disk created: $@ (ext2 $(EXT2_MB) MiB + Docker $(DOCKER_GB) GiB, sparse)"

# --- installs the programs into /bin and the system files (leaving everything else alone) ---
# SYSFILES: files from rootfs/ that the system manages and that are updated on every build
SYSFILES := etc/motd etc/profile etc/rc etc/mtab etc/zshrc etc/zprofile etc/shells etc/passwd \
	    etc/resolv.conf etc/hosts usr/share/vim/vimrc etc/tests.sh etc/netcfg.conf bin/nc
# Everything installed belongs to root (mke2fs -d keeps the build host's owners); the system
# directories get standard modes, and each home directory its user ("name:uid")
HOMES := user:1000
# EXTRA_BINS: ported programs (dash...), as "local/path:name"; LINKS: "name:target"
EXTRA_BINS ?=
LINKS ?=
install.stamp: $(DISK) licenses/.built $(SKARNET_BUILT) $(BINS) $(foreach b,$(EXTRA_BINS),$(firstword $(subst :, ,$(b)))) zsh/Src/zsh vim/src/vim procps/.built \
	       $(addprefix $(POSIX)/rootfs/,$(SYSFILES)) $(shell find $(POSIX)/rootfs/etc/s6-rc -type f)
	set -e; \
	python3 -c 'import fcntl,sys; fcntl.lockf(open(sys.argv[1],"r+b"), fcntl.LOCK_EX | fcntl.LOCK_NB)' $(DISK) 2>/dev/null \
		|| { echo "ERROR: $(DISK) is in use (QEMU running?): close it and run make again"; exit 1; }; \
	dd if=$(DISK) of=part.img bs=1M skip=1 count=$(EXT2_MB) status=none; \
	{ echo "mkdir /sbin"; echo "mkdir /mnt"; echo "mkdir /run"; \
	  for b in $(BINS) $(EXTRA_BINS); do \
	    src=$${b%%:*}; name=$$(basename $${b##*:}); dir=/bin; \
	    case " $(SBIN_APPS) " in *" $$name "*) dir=/sbin;; esac; \
	    echo "rm $$dir/$$name"; echo "write $$src $$dir/$$name"; echo "sif $$dir/$$name mode 0100755"; \
	    echo "sif $$dir/$$name uid 0"; echo "sif $$dir/$$name gid 0"; \
	  done; \
	  for l in $(LINKS); do echo "rm /bin/$${l%%:*}"; echo "symlink /bin/$${l%%:*} $${l##*:}"; done; \
	  for l in $(SBIN_LINKS); do echo "rm /sbin/$${l%%:*}"; echo "symlink /sbin/$${l%%:*} $${l##*:}"; done; \
	  for p in $$(cat coreutils/programs.txt); do echo "rm /bin/$$p"; echo "symlink /bin/$$p coreutils"; done; \
	  for f in $(patsubst %/.built,%/bin/*,$(SKARNET_BUILT)); do name=$$(basename $$f); \
	    echo "rm /bin/$$name"; echo "write $$f /bin/$$name"; echo "sif /bin/$$name mode 0100755"; \
	    echo "sif /bin/$$name uid 0"; echo "sif /bin/$$name gid 0"; done; \
	  echo "mkdir /usr/libexec"; for f in $(patsubst %/.built,%/libexec/*,$(SKARNET_BUILT)); do \
	    [ -f $$f ] || continue; name=$$(basename $$f); echo "rm /usr/libexec/$$name"; \
	    echo "write $$f /usr/libexec/$$name"; echo "sif /usr/libexec/$$name mode 0100755"; done; \
	  cat $(patsubst %/.built,%/links.txt,$(SKARNET_BUILT)) | while read name target; do \
	    echo "rm /bin/$$name"; echo "symlink /bin/$$name $$target"; done; \
	  for d in /usr /usr/share /usr/share/terminfo; do echo "mkdir $$d"; done; \
	  for t in $(TERMINFO); do echo "mkdir /usr/share/terminfo/$${t%%/*}"; \
	    echo "rm /usr/share/terminfo/$$t"; echo "write ncurses/inst/usr/share/terminfo/$$t /usr/share/terminfo/$$t"; done; \
	  for tr in $(DATA_TREES); do src=$${tr%%:*}; dst=$${tr##*:}; echo "mkdir $$(dirname $$dst)"; \
	    (cd $$src && find . -type d | sed "s|^\.|$$dst|; s|^|mkdir |"; \
	     find . -type f | while read f; do f=$${f#./}; echo "rm $$dst/$$f"; echo "write $$src/$$f $$dst/$$f"; done); done; \
	  for f in $(SYSFILES); do echo "rm /$$f"; echo "write $(POSIX)/rootfs/$$f /$$f"; \
	    echo "sif /$$f uid 0"; echo "sif /$$f gid 0"; done; \
	} > install.cmds.0; \
	awk '{ print } $$1 == "write" { print "sif " $$3 " uid 0"; print "sif " $$3 " gid 0" } \
	     $$1 == "mkdir" { print "sif " $$2 " uid 0"; print "sif " $$2 " gid 0" }' install.cmds.0 > install.cmds; \
	(cd $(POSIX)/rootfs && find . -mindepth 1 -type d | sed 's|^\.||') | while read d; do \
	  echo "sif $$d uid 0"; echo "sif $$d gid 0"; echo "sif $$d mode 040755"; done >> install.cmds; \
	printf '%s\n' "sif /tmp mode 041777" "sif /root mode 040700" $(foreach h,$(HOMES),"sif /home/$(word 1,$(subst :, ,$(h))) uid $(word 2,$(subst :, ,$(h)))" "sif /home/$(word 1,$(subst :, ,$(h))) gid $(word 2,$(subst :, ,$(h)))" "sif /home/$(word 1,$(subst :, ,$(h))) mode 040700") >> install.cmds; \
	debugfs -w -f install.cmds part.img >/dev/null 2>install.log || { cat install.log; exit 1; }; \
	dd if=part.img of=$(DISK) bs=1M seek=1 conv=notrunc status=none; \
	rm part.img; touch $@; \
	echo "installed in /bin: $(APPS) $(foreach b,$(EXTRA_BINS),$(lastword $(subst :, ,$(b)))) + $$(wc -l < coreutils/programs.txt) coreutils programs"

all: $(IMAGE_FILE) install.stamp
.PHONY: all sysroot
