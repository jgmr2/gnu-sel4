# GNU/seL4: sistema nativo (zsh, coreutils, red de FreeBSD...) + VM Linux con Docker, sobre seL4
#
#   make microkit-tool            (host)   herramienta de Microkit con nuestro parche (Rust en Docker)
#   make vm-rootfs                (host)   kernel + initramfs de la VM (busybox + Docker), con Docker
#   ../../dev.sh make             (Docker) todo: servidores, drivers, VMM, sysroot, programas, ports
#                                          -> build/; instala /bin en la partición ext2 de disk.img
#   make test                     (host)   pruebas automáticas en QEMU (sin pantalla, copia del disco)
#   make run                      (host)   arranca en QEMU+KVM con consola gráfica (salir: Ctrl-a, x
#                                          en esta terminal); CONSOLA=serie: por el puerto serie
#   ../../dev.sh make disk-reset  borra disk.img: se recrea (vacío) en el próximo make
#
# Consola: Ctrl-\ 1 Enter -> VM (Docker); Ctrl-\ 0 Enter -> GNU/seL4

export MICROKIT_SDK    ?= $(abspath ../../sdk/microkit-sdk-2.3.1)
export MICROKIT_BOARD  := x86_64_generic_vtx
export MICROKIT_CONFIG ?= debug
# Herramienta de Microkit propia (third_party/microkit, rama gnu-sel4: <cap_untyped>, <cap_cnode>)
export MICROKIT_TOOL   := $(abspath ../../third_party/microkit/target/release/microkit)
export LIBVMM          := $(abspath ../../libvmm)
export SDDF            := $(LIBVMM)/dep/sddf
export POSIX           := $(abspath .)
export BUILD_DIR       := $(abspath build)

IMAGE := $(BUILD_DIR)/loader.img
# Puertos del host reenviados: host:8080 -> VM:80, host:2222 -> VM:22,
# host:8090 -> GNU/seL4 nativo (pila de FreeBSD, 10.0.2.16):8080
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

# q35 con el disco en la ranura 3 y la red en la 4, y más de 4 GiB de RAM: los drivers de
# sDDF esperan las BAR e IRQ que el firmware asigna así (ver meta.py).
# Consola gráfica (por omisión): ventana de QEMU con pantalla y teclado; los mensajes del
# kernel y de los componentes siguen en esta terminal (Ctrl-a x cierra QEMU).
# make CONSOLA=serie: todo por el puerto serie, como antes (compilar con el mismo CONSOLA).
CONSOLA ?= grafica
ifeq ($(CONSOLA),serie)
PANTALLA := -nographic
else
PANTALLA := -display gtk -vga none -device bochs-display,addr=0x5
endif

run:
	@test -f $(IMAGE) || { echo "Falta $(IMAGE): compila con ../../dev.sh make"; exit 1; }
	qemu-system-x86_64 -machine q35 -accel kvm \
		-cpu host,+fsgsbase,+pdpe1gb,+xsaveopt,+xsave,+vmx,+vme \
		-kernel $(BUILD_DIR)/sel4_32.elf -initrd $(IMAGE) -m 6G $(PANTALLA) -serial mon:stdio \
		-drive file=disk.img,format=raw,if=none,id=hd -device virtio-blk-pci,drive=hd,addr=0x3.0 \
		-netdev user,id=net0,$(HOSTFWD) -device virtio-net-pci,netdev=net0,addr=0x4.0

# Pruebas automáticas en QEMU sin pantalla, sobre una copia del disco (tools/prueba.py).
# make test SIN_RED=1: sin las pruebas que necesitan internet
test:
	@test -f $(IMAGE) || { echo "Falta $(IMAGE): compila con ../../dev.sh make"; exit 1; }
	tools/prueba.py $(if $(SIN_RED),--sin-red)

disk-reset:
	rm -f disk.img build/install.stamp

# Une los discos de los proyectos anteriores (posix: ext2; docker-vm: Docker) en el
# formato de dos particiones, conservando su contenido. Se usa una vez.
clean:
	rm -rf build

.PHONY: all run test clean disk-reset vm-rootfs microkit-tool
