#!/bin/sh
# wasmkit/build-sysroot.sh <sysroot>: the sysroot for programs on the browser port (TODO item 24):
# musl ported to wasm32 (musl/port-musl.sh), the runtime (runtime/, crt0-wasm32.c) and sel4-cc.
# Run inside ../../dev.sh, after the ARM build (its generated syscall table, rt_sysmap.h).
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
POSIX=$(dirname "$HERE")
SYSROOT=$(mkdir -p "$1" && cd "$1" && pwd)
[ -f "$SYSROOT/lib/libc.a" ] || \
    "$POSIX/musl/port-musl.sh" "$POSIX/../../third_party/musl-1.2.6" "$SYSROOT/../musl-src" "$SYSROOT" wasm32
RT="clang --target=wasm32 -O2 -g -Wall -matomics -mbulk-memory -mexception-handling -mllvm -wasm-enable-sjlj
    -nostdinc -isystem $SYSROOT/include -I$POSIX/include -I$POSIX/build-qemu-arm"
O=$SYSROOT/rt-obj
mkdir -p "$O" "$SYSROOT/bin"
# shellcheck disable=SC2086
for f in start syscall crt0-wasm32; do $RT -c "$POSIX/runtime/$f.c" -o "$O/$f.o"; done
cp "$O/crt0-wasm32.o" "$SYSROOT/lib/crt0.o"
rm -f "$SYSROOT/lib/libsel4posix.a"
llvm-ar rcs "$SYSROOT/lib/libsel4posix.a" "$O/start.o" "$O/syscall.o"
cp "$POSIX/tools/sel4-cc" "$SYSROOT/bin/sel4-cc"
echo wasm32 > "$SYSROOT/arch"
echo "sysroot ready: $SYSROOT"
