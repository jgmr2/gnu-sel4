#!/bin/sh
# wasmkit/build.sh <disk.img> <out-dir>: experiment 1 of the browser port (TODO item 24): fs_ext2
# and a client compiled to WebAssembly, run by wasmkit (Microkit's API on Web Workers), with
# partition 1 of <disk.img> as fs_ext2's RAM disk. Run inside ../../dev.sh (clang, wasm-ld).
# Serve <out-dir> with tools/wasm/serve.py (COOP/COEP: shared memory).
#
# The one shared memory, laid out by hand for now (system.json tells wasmkit.js):
#   0x00010000  wasmkit's control blocks (wasmkit.h)
#   0x01000000  fs_ext2      0x03000000  bench      (each linked at its base: --global-base)
#   0x04000000  fs_buf (1 MiB)    0x05000000  the RAM disk
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
LDFLAGS="--no-entry --import-memory --shared-memory --max-memory=1073741824 -z stack-size=65536
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
    wasm-ld $LDFLAGS --global-base="$((base))" $exports $objs -o "$OUT/$name.wasm"
}
LIBC=$HERE/libc.c
pd fs_ext2 0x01000000 "fs_buf ramdisk_start ramdisk_end" "$POSIX/fs/ext2/fs_ext2.c" "$HERE/wasmkit.c" "$LIBC"
pd bench 0x03000000 "fs_buf" "$HERE/bench.c" "$HERE/wasmkit.c" "$LIBC"
rm -f "$OUT"/*.o

# The RAM disk: partition 1 of the disk (its MBR entry)
start=$(python3 -c 'import sys; m=open(sys.argv[1],"rb").read(512); print(int.from_bytes(m[454:458],"little"))' "$DISK")
count=$(python3 -c 'import sys; m=open(sys.argv[1],"rb").read(512); print(int.from_bytes(m[458:462],"little"))' "$DISK")
dd if="$DISK" of="$OUT/part1.img" bs=512 skip="$start" count="$count" status=none
cp "$HERE/site/index.html" "$HERE/site/wasmkit.js" "$HERE/site/worker.js" "$HERE/system.json" "$OUT/"
ls -la "$OUT"
