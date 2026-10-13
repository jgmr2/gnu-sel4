#!/bin/sh
# wasmkit/build.sh <disk.img> <out-dir>: the browser port (TODO item 24): GNU/seL4's components
# compiled to WebAssembly, run by wasmkit (Microkit's API on Web Workers), with partition 1 of
# <disk.img> as fs_ext2's RAM disk. Run inside ../../dev.sh (clang, wasm-ld, debugfs), after the
# ARM build (its generated syscall tables). Serve <out-dir> with tools/wasm/serve.py (COOP/COEP:
# shared memory). system.json: posix_server (1b); bench.json (?system=bench): experiment 1a.
#
# The one shared memory, laid out by hand for now: layout.h (and system.json, bench.json).
# Each component is linked at its base (--global-base).
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
POSIX=$(dirname "$HERE")
DISK=$1; OUT=$2
SDDF=${SDDF:-$POSIX/../../libvmm/dep/sddf}
mkdir -p "$OUT"
CFLAGS="--target=wasm32 -O2 -g -matomics -mbulk-memory -ffreestanding -nostdlib -fno-builtin-printf
    -Wall -Wno-unused-function -I$HERE/include -I$POSIX/include -I$SDDF/include -I$SDDF/include/microkit
    -I$SDDF/include/sddf/util/custom_libc -DFS_RAMDISK"
LDFLAGS="--no-entry --import-memory --shared-memory --max-memory=1073741824
    --export=__heap_base --export=__data_end"

pd() {   # pd <name> <base> <exported setvars> <sources>...
    name=$1 base=$2 vars=$3; shift 3
    objs=""
    for src in "$@"; do
        o="$OUT/$name-$(basename "$src" .c).o"
        # shellcheck disable=SC2086
        clang $CFLAGS -c "$src" -o "$o"
        objs="$objs $o"
    done
    exports=""
    for v in $vars; do exports="$exports --export=$v"; done
    # shellcheck disable=SC2086
    wasm-ld $LDFLAGS -z stack-size="${STACK:-65536}" --global-base="$((base))" $exports $objs -o "$OUT/$name.wasm"
}
LIBC=$HERE/libc.c
pd fs_ext2 0x01000000 "fs_buf ramdisk_start ramdisk_end" "$POSIX/fs/ext2/fs_ext2.c" "$HERE/wasmkit.c" "$LIBC"
pd bench 0x03000000 "fs_buf" "$HERE/bench.c" "$HERE/wasmkit.c" "$LIBC"
pd console 0x02000000 "" "$HERE/console.c" "$HERE/wasmkit.c" "$LIBC"
pd timer 0x02400000 "" "$HERE/timer.c" "$HERE/wasmkit.c" "$LIBC"
# posix_server: all of it but mem.c (mem_wasm.c instead), the VM bridge, the network and
# Microkit's main loop (wasmkit's); the syscall tables the ARM build generated
SRV=""
for f in "$POSIX"/server/*.c; do
    case $f in */memcpy.c|*/microkit_main.c|*/mem.c) continue;; esac
    SRV="$SRV $f"
done
CFLAGS="$CFLAGS -I$POSIX/build-qemu-arm -DNO_NETSTACK -DNO_ACPI_POWEROFF ${SRV_FLAGS:-}" \
STACK=1048576 pd posix_server 0x03000000 "fs_buf exec_buf proc_bufs" $SRV "$HERE/server_conf.c" \
    "$HERE/wasmkit.c" "$LIBC" "$SDDF/util/printf.c"
rm -f "$OUT"/*.o

# Programs: the sysroot (musl for wasm32, the runtime, sel4-cc), then init and a test (hello)
"$HERE/build-sysroot.sh" "$OUT/../sysroot" >/dev/null
for p in init hello; do "$OUT/../sysroot/bin/sel4-cc" -O2 "$HERE/$p.c" -o "$OUT/$p.wasm"; done

# The RAM disk: partition 1 of the disk (its MBR entry)
start=$(python3 -c 'import sys; m=open(sys.argv[1],"rb").read(512); print(int.from_bytes(m[454:458],"little"))' "$DISK")
count=$(python3 -c 'import sys; m=open(sys.argv[1],"rb").read(512); print(int.from_bytes(m[458:462],"little"))' "$DISK")
dd if="$DISK" of="$OUT/part1.img" bs=512 skip="$start" count="$count" status=none
# The programs built for wasm32 in place of the disk's ARM ones (their links stay): /sbin/init,
# /bin/hello, and the ports when they are built (SEL4_HOST=wasm32-linux-musl with their port
# scripts, in build-wasm/<port>), without their debug information
install_prog() {   # install_prog <file> <path>
    printf 'rm %s\nwrite %s %s\nsif %s mode 0100755\nsif %s uid 0\nsif %s gid 0\n' "$2" "$1" "$2" "$2" "$2" "$2" |
        debugfs -w -f - "$OUT/part1.img" >/dev/null 2>&1
}
install_prog "$OUT/init.wasm" /sbin/init
install_prog "$OUT/hello.wasm" /bin/hello
for p in coreutils/src/coreutils:/bin/coreutils dash/src/dash:/bin/dash; do
    [ -f "$OUT/../${p%%:*}" ] || continue
    llvm-objcopy --strip-debug "$OUT/../${p%%:*}" "$OUT/$(basename "${p%%:*}").wasm"
    install_prog "$OUT/$(basename "${p%%:*}").wasm" "${p##*:}"
done
cp "$HERE/site/index.html" "$HERE/site/wasmkit.js" "$HERE/site/worker.js" "$HERE/site/proc.js" \
    "$HERE/system.json" "$HERE/bench.json" "$OUT/"
ls -la "$OUT"
