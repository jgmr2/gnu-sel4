#!/bin/sh
# FreeBSD TCP/IP stack (from F-Stack, third_party/f-stack) for GNU/seL4.
# Builds the FreeBSD part (sys/kern, net, netinet, netinet6...) for bare x86_64 or aarch64, into a single
# relocatable object with only the ff_* API visible. F-Stack's "host" part (DPDK, Linux)
# is not used: the netstack component replaces it (projects/gnu-sel4/net/).
#   Usage: build-fstack.sh <output dir> [x86_64|aarch64]      Output: <dir>/fstack.o
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
OUT=$(realpath "$1"); ARCH=${2:-x86_64}
SRC=$(realpath "${FSTACK:-$(dirname "$0")/../../../../third_party/f-stack}")
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

# arm64 (F-Stack is mostly used on amd64): its lib/include/machine/pcpu.h reaches the CPU's data
# through pc_prvspace, a field only amd64's struct pcpu has (added here, out of the padding), and
# a global pcpup (arm64 binds it to register x18: clang makes it a symbol in every file); and
# sys/libkern.h has fls/flsl inline, which F-Stack's arm64 list compiles again
if [ "${2:-x86_64}" = aarch64 ]; then
    cat > pcpu-arm64.sh <<'SH'
sed -i -e 's/^register struct pcpu \*pcpup __asm ("x18");/extern struct pcpu *pcpup;/' -e 's/^\tu_int\tpc_acpi_id;/\tstruct pcpu *pc_prvspace;\t\\\n&/' -e 's/char __pad\[201\]/char __pad[193]/' "$1"
grep -q pc_prvspace "$1"
SH
    sed -i 's|^\t@cp -r $S/${MACHINE_CPUARCH}/include/\* ${MACHINE_INCLUDES_ROOT}/machine$|\0\n\t@sh $(CURDIR)/pcpu-arm64.sh ${MACHINE_INCLUDES_ROOT}/machine/pcpu.h|' Makefile.sel4
    sed -i 's/^fstack.o:/OBJS:=$(filter-out fls.o flsl.o,$(OBJS))\n\0/' Makefile.sel4
    # UMA's page -> slab hash is made after uma_startup1/2, but on arm64 (128-byte cache lines:
    # other slab layouts) a zone made during startup already uses it: made before (a local copy
    # of ff_freebsd_init.c, found before VPATH's)
    awk '/^    num_hash_buckets = 8192;$/ || /^    uma_page_slab_hash = / || /^    uma_page_mask = / { next }
         /^    boot_pages = 16;$/ { print "    num_hash_buckets = 8192;"
             print "    uma_page_slab_hash = (struct uma_page_head *)kmem_malloc(sizeof(struct uma_page)*num_hash_buckets, M_ZERO);"
             print "    uma_page_mask = num_hash_buckets - 1;" }
         { print }' "$SRC/lib/ff_freebsd_init.c" > ff_freebsd_init.c
    [ "$(grep -c 'uma_page_slab_hash = ' ff_freebsd_init.c)" = 1 ] || { echo "build-fstack.sh: ff_freebsd_init.c patch failed" >&2; exit 1; }
fi
case $ARCH in
    x86_64)  CPU=amd64; AFLAGS="-mno-red-zone -mcmodel=small" ;;
    aarch64) CPU=arm64; AFLAGS="-mstrict-align" ;;
    *) echo "build-fstack.sh: unknown architecture $ARCH" >&2; exit 1 ;;
esac
make -f Makefile.sel4 TOPDIR="$SRC" MACHINE_CPUARCH=$CPU COMPILER_TYPE=clang \
     CC="clang -target $ARCH-none-elf -ffreestanding $AFLAGS -fno-pic -fno-stack-protector -D__thread=" \
     LD=ld.lld WERROR="-Wno-error" -j"$(nproc)" fstack.o
