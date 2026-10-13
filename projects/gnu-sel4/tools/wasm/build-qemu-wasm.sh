#!/bin/sh
# build-qemu-wasm.sh <out-dir>: QEMU for aarch64 compiled to WebAssembly (ktock/qemu-wasm, its
# TCG backend that turns translated blocks into wasm), for the browser demo (TODO item 22).
# The ARM build of GNU/seL4 needs 2 GiB of guest RAM (the SDK kernel's memory map), so the
# heap goes to wasm32's 4 GiB. Pinned to one commit; built in its own emscripten image.
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
OUT=$(realpath "$1")
COMMIT=0ef7b4e2814b231705d8371dd7997f5b72e70baf
SRC=$(dirname "$0")/../../../../third_party/qemu-wasm
mkdir -p "$OUT"
if [ ! -d "$SRC/.git" ] || [ "$(git -C "$SRC" rev-parse HEAD)" != "$COMMIT" ]; then
    rm -rf "$SRC" && mkdir -p "$SRC"
    git -C "$SRC" init -q && git -C "$SRC" remote add origin https://github.com/ktock/qemu-wasm
    git -C "$SRC" fetch -q --depth 1 origin "$COMMIT" && git -C "$SRC" checkout -q FETCH_HEAD
fi
SRC=$(realpath "$SRC")   # writable: meson fetches dtc (libfdt) into subprojects/
# zlib.net keeps only its latest release: this one from its author's GitHub releases
sed 's|https://zlib.net/zlib-|https://github.com/madler/zlib/releases/download/v1.3.1/zlib-|' "$SRC/Dockerfile" | docker build -t gnu-sel4-qemu-wasm-build -
docker rm -f gnu-sel4-qemu-wasm >/dev/null 2>&1 || true
docker run --rm -d --name gnu-sel4-qemu-wasm -v "$SRC":/qemu/ -v "$OUT":/out gnu-sel4-qemu-wasm-build
docker exec gnu-sel4-qemu-wasm sh -c '
set -e
export EXTRA_CFLAGS="-O3 -g -Wno-error=unused-command-line-argument -matomics -mbulk-memory -DNDEBUG -DG_DISABLE_ASSERT -D_GNU_SOURCE -sASYNCIFY=1 -pthread -sPROXY_TO_PTHREAD=1 -sFORCE_FILESYSTEM -sALLOW_TABLE_GROWTH -sINITIAL_MEMORY=2800MB -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4294901760 -sWASM_BIGINT -sMALLOC=mimalloc --js-library=/build/node_modules/xterm-pty/emscripten-pty.js -sEXPORT_ES6=1 -sASYNCIFY_IMPORTS=ffi_call_js"
export EXTRA_LDFLAGS="$EXTRA_CFLAGS"
mkdir -p /build && cd /build
emconfigure /qemu/configure --static --target-list=aarch64-softmmu --cpu=wasm32 --cross-prefix= \
    --without-default-features --enable-system --with-coroutine=fiber --enable-virtfs \
    --extra-cflags="$EXTRA_CFLAGS" --extra-cxxflags="$EXTRA_CFLAGS" --extra-ldflags="$EXTRA_LDFLAGS"
emmake make -j$(nproc) qemu-system-aarch64
cp qemu-system-aarch64 qemu-system-aarch64.wasm qemu-system-aarch64.worker.js /out/
'
docker rm -f gnu-sel4-qemu-wasm >/dev/null
ls -la "$OUT"
