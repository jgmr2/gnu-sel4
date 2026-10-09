#
# Reglas de compilación de GNU/seL4 (se copian a build/Makefile y se ejecutan ahí, en Docker).
# Variables de entrada: MICROKIT_SDK MICROKIT_BOARD MICROKIT_CONFIG SDDF LIBVMM POSIX BUILD_DIR
#
SUPPORTED_BOARDS := x86_64_generic_vtx
TOOLCHAIN ?= clang
SDDF_CUSTOM_LIBC := 1
include $(SDDF)/tools/make/board/common.mk

SYSTEM_FILE := gnu-sel4.system
IMAGE_FILE  := loader.img
REPORT_FILE := report.txt

# --- componentes de sDDF: consola (COM1), disco y red (virtio), temporizador, con sus virtualizadores ---
# Los drivers virtio no leen el espacio de configuración PCI: las BAR de q35 son fijas (ver meta.py)
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

# --- tablas de syscalls generadas desde musl (números y nombres de Linux x86_64) ---
MUSL_SRC := $(POSIX)/../../third_party/musl-1.2.6
syscall_nr.h: $(MUSL_SRC)/arch/x86_64/bits/syscall.h.in
	cp $< $@
syscall_names.h: $(MUSL_SRC)/arch/x86_64/bits/syscall.h.in
	{ echo 'static const char *const syscall_names[] = {'; \
	  sed -n 's/^#define __NR_\([a-z0-9_]*\)[[:space:]]*\([0-9]*\)$$/  [\2] = "\1",/p' $<; \
	  echo '};'; } > $@

# --- consola gráfica: pantalla + teclado PS/2 como "driver serial" de sDDF (console/) ---
SPLEEN := $(POSIX)/../../third_party/spleen/spleen-8x16.bdf
font_spleen.h: $(SPLEEN) $(POSIX)/tools/bdf2c.py
	python3 $(POSIX)/tools/bdf2c.py $< $@
console_%.o: $(POSIX)/console/%.c $(POSIX)/console/term.h font_spleen.h
	$(CC) $(CFLAGS) -I. -c $< -o $@
fbcon.elf: console_fbcon.o console_term.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# --- servidor POSIX: despacho + procesos + terminal + VFS ---
SERVER_SRCS := posix_server proc tty vfs clock bridge mem procfs
SERVER_DEPS := $(wildcard $(POSIX)/server/*.h) $(wildcard $(POSIX)/include/sel4posix/*.h) \
	       syscall_nr.h syscall_names.h
server_%.o: $(POSIX)/server/%.c $(SERVER_DEPS)
	$(CC) $(CFLAGS) -I$(POSIX)/include -I. -c $< -o $@
posix_server.elf: $(addprefix server_,$(addsuffix .o,$(SERVER_SRCS)))
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# --- servidor de archivos ext2 ---
fs_ext2.o: $(POSIX)/fs/ext2/fs_ext2.c $(POSIX)/include/sel4posix/fs_protocol.h
	$(CC) $(CFLAGS) -I$(POSIX)/include -DBUILD_TIME=$(shell date +%s) -c $< -o $@
fs_ext2.elf: fs_ext2.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# --- VMM de la máquina virtual (libvmm): VM Linux mínima con Docker ---
VM := $(POSIX)/vm
VM_KERNEL := $(VM)/rootfs/out/vmlinuz
VM_INITRD := $(VM)/rootfs/out/initramfs.cpio.gz
$(VM_KERNEL) $(VM_INITRD):
	@echo "Falta el rootfs de la VM: ejecuta 'make vm-rootfs' en el host"; exit 1
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

# --- sysroot para programas: musl portada + runtime + sel4-cc ---
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

# --- netstack: pila TCP/IP de FreeBSD (F-Stack) como componente, con la libc de musl ---
FSTACK := $(POSIX)/../../third_party/f-stack
fstack/fstack.o: $(POSIX)/ports/fstack/build-fstack.sh
	$< fstack > fstack.log 2>&1 || { tail -20 fstack.log; exit 1; }
NET_CFLAGS := --target=x86_64-none-elf -O2 -g -Wall -fno-pic -fno-stack-protector -mfsgsbase -mrdrnd \
	      -nostdinc -isystem $(SYSROOT)/include -I$(BOARD_DIR)/include -I$(SDDF)/include \
	      -I$(SDDF)/include/microkit -I$(FSTACK)/lib -I$(POSIX)/include -I$(POSIX)/net/compat -D_GNU_SOURCE -D__thread= -include fstack_host.h -DINET -DINET6
net_%.o: $(POSIX)/net/%.c $(POSIX)/net/netstack.h $(LIBC)
	clang $(NET_CFLAGS) -c $< -o $@
net_tsc.o: $(SDDF)/util/arch_timestamp_counter.c $(LIBC)
	clang $(NET_CFLAGS) -c $< -o $@
netstack.elf: net_netstack.o net_host.o net_tsc.o fstack/fstack.o $(LIBC)
	$(LD) $(LDFLAGS) net_netstack.o net_host.o net_tsc.o fstack/fstack.o \
		--start-group -lmicrokit -Tmicrokit.ld $(LIBC) --end-group -o $@
# segundo copiador de red de sDDF (cada uno lleva su configuración dentro del ELF)
netstack_copy.elf: network_copy.elf
	cp $< $@

# --- programas de apps/<nombre>/*.c -> bin/<nombre> (en /bin, o en /sbin los de SBIN_APPS) ---
SBIN_APPS := init 9pd
APPS := $(notdir $(wildcard $(POSIX)/apps/*))
BINS := $(addprefix bin/,$(APPS))
.SECONDEXPANSION:
bin/%: $(SYSROOT_READY) $$(wildcard $(POSIX)/apps/%/*.c)
	mkdir -p bin
	$(SEL4CC) -O2 -g -Wall -I$(POSIX)/include -o $@ $(wildcard $(POSIX)/apps/$*/*.c)

# --- programas portados ---
# Todos dependen de $(SYSROOT_READY): si cambia el runtime (libsel4posix.a) o musl, se
# recompilan y vuelven a enlazar (si no, quedarían con el runtime viejo dentro).
DASH_SRC := $(POSIX)/../../third_party/dash-0.5.13.5
dash/src/dash: $(SYSROOT_READY) $(POSIX)/ports/dash/build-dash.sh $(POSIX)/ports/dash/mksignames.c
	$(POSIX)/ports/dash/build-dash.sh $(DASH_SRC) dash $(SYSROOT)

# GNU coreutils: un binario multi-llamada; sus programas se enlazan en el install
CU_SRC := $(POSIX)/../../third_party/coreutils-9.12
coreutils/src/coreutils: $(SYSROOT_READY) $(POSIX)/ports/coreutils/build-coreutils.sh
	$(POSIX)/ports/coreutils/build-coreutils.sh $(CU_SRC) coreutils $(SYSROOT)
coreutils.bin: coreutils/src/coreutils
	llvm-strip -o $@ $<

# ncurses: biblioteca en el sysroot (la usan zsh y vim) + sus programas (clear, tput, reset...)
NC_SRC := $(POSIX)/../../third_party/ncurses-6.6
NCURSES := ncurses/.compilado
$(NCURSES): $(SYSROOT_READY) $(POSIX)/ports/ncurses/build-ncurses.sh
	$(POSIX)/ports/ncurses/build-ncurses.sh $(NC_SRC) ncurses $(SYSROOT)
	touch $@
NC_PROGS := clear tput reset tset infocmp tic toe tabs
ncurses/inst/usr/bin/%: $(NCURSES)
	@test -f $@

# zsh (estático, módulos dentro del binario)
ZSH_SRC := $(POSIX)/../../third_party/zsh-5.9.2
zsh/Src/zsh: $(NCURSES) $(SYSROOT_READY) $(POSIX)/ports/zsh/build-zsh.sh
	$(POSIX)/ports/zsh/build-zsh.sh $(ZSH_SRC) zsh $(SYSROOT)

# vim (modo texto) con un runtime reducido
VIM_SRC := $(POSIX)/../../third_party/vim-9.2.1172
vim/src/vim: $(NCURSES) $(SYSROOT_READY) $(POSIX)/ports/vim/build-vim.sh
	$(POSIX)/ports/vim/build-vim.sh $(VIM_SRC) vim $(SYSROOT)
vim.bin: vim/src/vim
	llvm-strip -o $@ $<

# procps-ng: ps, top, free... leen el /proc de posix_server (server/procfs.c)
PROCPS_SRC := $(POSIX)/../../third_party/procps-ng-4.0.7
PROCPS_PROGS := ps top free uptime pgrep pkill pidof watch vmstat pwdx tload
procps/.compilado: $(NCURSES) $(SYSROOT_READY) $(POSIX)/ports/procps/build-procps.sh
	$(POSIX)/ports/procps/build-procps.sh $(PROCPS_SRC) procps $(SYSROOT)
	mkdir -p procps/bin && for p in $(PROCPS_PROGS); do llvm-strip -o procps/bin/$$p procps/inst/usr/local/bin/$$p; done
	touch $@
$(addprefix procps/bin/,$(PROCPS_PROGS)): procps/.compilado ;

EXTRA_BINS := dash/src/dash:dash coreutils.bin:coreutils zsh/Src/zsh:zsh vim.bin:vim \
	      $(foreach p,$(NC_PROGS),ncurses/inst/usr/bin/$(p):$(p)) \
	      $(foreach p,$(PROCPS_PROGS),procps/bin/$(p):$(p))
# Árboles de datos que se copian al disco: "origen:destino"
DATA_TREES := zsh/inst/usr/share/zsh:/usr/share/zsh vim/runtime-min:/usr/share/vim/vim92
# Enlaces: "nombre:destino". Los de coreutils salen de coreutils/programas.txt
# Comandos de la VM Linux disponibles como nativos (puente, como el interop de WSL)
LINUX_CMDS := docker
LINKS := sh:dash vi:vim $(foreach c,$(LINUX_CMDS),$(c):linux)
# Restos de versiones anteriores que se quitan del disco
OBSOLETE := /bin/utiles /bin/hola /bin/eco /bin/minish
# terminfo: solo las terminales que se usan (la base completa tiene ~1900)
TERMINFO := x/xterm x/xterm-256color x/xterm-color v/vt100 v/vt102 v/vt220 l/linux s/screen s/screen-256color d/dumb a/ansi

# --- descripción del sistema + configuración de sDDF inyectada en cada ELF ---
# Cambiar de consola (make CONSOLA=serie|grafica) regenera la descripción del sistema
CONSOLA ?= grafica
export CONSOLA
consola.$(CONSOLA):
	rm -f consola.* && touch $@
$(SYSTEM_FILE): $(POSIX)/meta.py $(IMAGES) consola.$(CONSOLA)
	PYTHONPATH=$(SDDF)/tools/meta:$$PYTHONPATH python3 $(POSIX)/meta.py --sddf $(SDDF) \
		--board $(MICROKIT_BOARD) --output . --sdf $@
	# regiones grandes (RAM de la VM, memoria de netstack) en páginas de 2 MiB (sdfgen no tiene esa opción)
	sed -i -E 's|(<memory_region name="(guest_ram\|netstack_heap)" size="0x[0-9a-f]+") />|\1 page_size="0x200000" />|' $@
	grep -q 'name="guest_ram".*page_size' $@
	if [ "$(CONSOLA)" = serie ]; then \
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

# --- disco: MBR con dos particiones ---
#   1: ext2 de GNU/seL4 (contenido inicial: rootfs/)     -> fs_ext2
#   2: /var/lib/docker de la VM (la VM la formatea en ext4) -> CLIENT_VMM
# Se crea una sola vez (persistente). Para recrearlo: make disk-reset
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
	echo "disco creado: $@ (ext2 $(EXT2_MB) MiB + Docker $(DOCKER_GB) GiB, disperso)"

# --- instala los programas en /bin y los archivos de sistema (sin tocar el resto) ---
# SYSFILES: archivos de rootfs/ que administra el sistema y se actualizan en cada build
SYSFILES := etc/motd etc/profile etc/rc etc/mtab etc/zshrc etc/zprofile etc/shells etc/passwd \
	    etc/resolv.conf etc/hosts usr/share/vim/vimrc
# EXTRA_BINS: programas portados (dash...), como "ruta/local:nombre"; LINKS: "nombre:destino"
EXTRA_BINS ?=
LINKS ?=
install.stamp: $(DISK) $(BINS) $(foreach b,$(EXTRA_BINS),$(firstword $(subst :, ,$(b)))) zsh/Src/zsh vim/src/vim procps/.compilado \
	       $(addprefix $(POSIX)/rootfs/,$(SYSFILES))
	set -e; \
	python3 -c 'import fcntl,sys; fcntl.lockf(open(sys.argv[1],"r+b"), fcntl.LOCK_EX | fcntl.LOCK_NB)' $(DISK) 2>/dev/null \
		|| { echo "ERROR: $(DISK) está en uso (¿QEMU abierto?): ciérralo y vuelve a ejecutar make"; exit 1; }; \
	dd if=$(DISK) of=part.img bs=1M skip=1 count=$(EXT2_MB) status=none; \
	{ echo "mkdir /sbin"; echo "mkdir /mnt"; \
	  for b in $(BINS) $(EXTRA_BINS); do \
	    src=$${b%%:*}; name=$$(basename $${b##*:}); dir=/bin; \
	    case " $(SBIN_APPS) " in *" $$name "*) dir=/sbin;; esac; \
	    echo "rm $$dir/$$name"; echo "write $$src $$dir/$$name"; echo "sif $$dir/$$name mode 0100755"; \
	    echo "sif $$dir/$$name uid 0"; echo "sif $$dir/$$name gid 0"; \
	  done; \
	  for l in $(LINKS); do echo "rm /bin/$${l%%:*}"; echo "symlink /bin/$${l%%:*} $${l##*:}"; done; \
	  for p in $$(cat coreutils/programas.txt); do echo "rm /bin/$$p"; echo "symlink /bin/$$p coreutils"; done; \
	  for o in $(OBSOLETE); do echo "rm $$o"; done; \
	  for d in /usr /usr/share /usr/share/terminfo; do echo "mkdir $$d"; done; \
	  for t in $(TERMINFO); do echo "mkdir /usr/share/terminfo/$${t%%/*}"; \
	    echo "rm /usr/share/terminfo/$$t"; echo "write ncurses/inst/usr/share/terminfo/$$t /usr/share/terminfo/$$t"; done; \
	  for tr in $(DATA_TREES); do src=$${tr%%:*}; dst=$${tr##*:}; echo "mkdir $$(dirname $$dst)"; \
	    (cd $$src && find . -type d | sed "s|^\.|$$dst|; s|^|mkdir |"; \
	     find . -type f | while read f; do f=$${f#./}; echo "rm $$dst/$$f"; echo "write $$src/$$f $$dst/$$f"; done); done; \
	  for f in $(SYSFILES); do echo "rm /$$f"; echo "write $(POSIX)/rootfs/$$f /$$f"; \
	    echo "sif /$$f uid 0"; echo "sif /$$f gid 0"; done; \
	} > install.cmds; \
	debugfs -w -f install.cmds part.img >/dev/null 2>install.log || { cat install.log; exit 1; }; \
	dd if=part.img of=$(DISK) bs=1M seek=1 conv=notrunc status=none; \
	rm part.img; touch $@; \
	echo "instalado en /bin: $(APPS) $(foreach b,$(EXTRA_BINS),$(lastword $(subst :, ,$(b)))) + $$(wc -l < coreutils/programas.txt) programas de coreutils"

all: $(IMAGE_FILE) install.stamp
.PHONY: all sysroot
