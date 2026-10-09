#!/bin/sh
# Pila TCP/IP de FreeBSD (de F-Stack, third_party/f-stack) para GNU/seL4.
# Compila la parte de FreeBSD (sys/kern, net, netinet, netinet6...) para x86_64 sin SO, en un
# solo objeto reubicable con solo la API ff_* visible. La parte "host" de F-Stack (DPDK,
# Linux) no se usa: la reemplaza el componente netstack (proyectos/gnu-sel4/net/).
#   Uso: build-fstack.sh <dir de salida>      Salida: <dir>/fstack.o
set -eu
OUT=$(realpath "$1")
SRC=$(realpath "$(dirname "$0")/../../../../third_party/f-stack")
mkdir -p "$OUT"
cd "$OUT"

# Makefile de F-Stack sin DPDK ni gcc, con fuentes y cabeceras propias en $SRC/lib
sed -e '/pkg-config --exists libdpdk/,/^endif/d' \
    -e 's/^DPDK_CFLAGS=.*/DPDK_CFLAGS=/' \
    -e 's/^FF_KNI=1/#FF_KNI=1/' \
    -e '/^GCCVERGE/,/^endif/d' \
    -e 's|^MACHINE_INCLUDES_ROOT:=.*|MACHINE_INCLUDES_ROOT:=$(CURDIR)/machine_include|' \
    -e 's|^OVERRIDE_INCLUDES_ROOT:=.*|OVERRIDE_INCLUDES_ROOT:=$(TOPDIR)/lib/include|' \
    -e 's|^INCLUDES+= -I./opt|INCLUDES+= -I$(TOPDIR)/lib/opt -I$(TOPDIR)/lib|' \
    "$SRC/lib/Makefile" > Makefile.sel4
echo 'VPATH+= $(TOPDIR)/lib' >> Makefile.sel4
cat >> Makefile.sel4 <<'MK'
# Solo la parte de FreeBSD, en un objeto con la API ff_* como únicos símbolos globales
fstack.o: machine_includes ${MHEADERS} ${MSRCS} ${OBJS}
	${LD} -r -o fstack.ro ${OBJS}
	llvm-nm fstack.ro | grep -v ' U ' | cut -d ' ' -f 3 > localize.tmp
	llvm-objcopy --localize-symbols=localize.tmp fstack.ro
	llvm-objcopy --globalize-symbols=$(TOPDIR)/lib/ff_api.symlist fstack.ro
	mv fstack.ro fstack.o
MK

make -f Makefile.sel4 TOPDIR="$SRC" MACHINE_CPUARCH=amd64 COMPILER_TYPE=clang \
     CC="clang -target x86_64-none-elf -ffreestanding -mno-red-zone -mcmodel=small -fno-pic -fno-stack-protector -D__thread=" \
     LD=ld.lld WERROR="-Wno-error" -j"$(nproc)" fstack.o
