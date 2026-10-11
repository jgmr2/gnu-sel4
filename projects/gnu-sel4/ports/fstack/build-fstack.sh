#!/bin/sh
# FreeBSD TCP/IP stack (from F-Stack, third_party/f-stack) for GNU/seL4.
# Builds the FreeBSD part (sys/kern, net, netinet, netinet6...) for bare x86_64, into a single
# relocatable object with only the ff_* API visible. F-Stack's "host" part (DPDK, Linux)
# is not used: the netstack component replaces it (projects/gnu-sel4/net/).
#   Usage: build-fstack.sh <output dir>      Output: <dir>/fstack.o
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
OUT=$(realpath "$1")
SRC=$(realpath "$(dirname "$0")/../../../../third_party/f-stack")
mkdir -p "$OUT"
cd "$OUT"

# F-Stack's Makefile without DPDK or gcc, with our own sources and headers in $SRC/lib
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
# Only the FreeBSD part, in one object with the ff_* API as the only global symbols
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
