#!/bin/sh
# make-site.sh <qemu-wasm-dir> <loader.img> <demo-disk.img> <site-dir> [<vm.state>]: the browser
# demo's files (TODO item 22): qemu-wasm, the page, and the images packed with emscripten's file
# packager, compressed (<demo-disk.img> -: no disk). With a snapshot (make-snapshot.sh: its
# disk.img and vm.state) the page starts from it, at the login prompt, instead of booting.
# Serve <site-dir> with serve.py (COOP/COEP headers: SharedArrayBuffer needs them).
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
QW=$(realpath "$1"); LOADER=$(realpath "$2"); DISKIMG=$3; SITE=$4; STATE=${5:-}
HERE=$(cd "$(dirname "$0")" && pwd)
rm -rf "$SITE" && mkdir -p "$SITE/pack"
cp "$HERE/site/index.html" "$HERE/site/module.js" "$SITE/"
cp "$QW/qemu-system-aarch64" "$SITE/qemu-system-aarch64.js"
# The shared memory's maximum: 65535 pages, what the TCG backend's per-block modules declare
# (emscripten writes 65536 for 4 GiB and those modules then fail to link)
sed -i 's|"maximum": 4294967296 / 65536,|"maximum": 4294901760 / 65536,|' "$SITE/qemu-system-aarch64.js"
cp "$QW/qemu-system-aarch64.wasm" "$QW/qemu-system-aarch64.worker.js" "$SITE/"
cp "$LOADER" "$SITE/pack/loader.img"
if [ "$DISKIMG" = - ]; then     # no disk (a system without one: Microkit's hello)
    echo "for (const o of ['-drive', '-device']) { const a = Module['arguments'], i = a.findIndex((x, j) => x === o && /hd/.test(a[j + 1])); a.splice(i, 2); }" >> "$SITE/module.js"
else
    cp "$DISKIMG" "$SITE/pack/disk.img"
fi
if [ -n "$STATE" ]; then
    cp "$STATE" "$SITE/pack/vm.state"
    echo "Module['arguments'].push('-incoming', 'file:/pack/vm.state');" >> "$SITE/module.js"
fi
SITE=$(realpath "$SITE")
docker run --rm -v "$SITE":/site -w /site gnu-sel4-qemu-wasm-build \
    sh -c '/emsdk/upstream/emscripten/tools/file_packager.py qemu-system-aarch64.data --preload /site/pack@/pack > load.js'
rm -rf "$SITE/pack"
gzip -6 "$SITE/qemu-system-aarch64.data" "$SITE/qemu-system-aarch64.wasm"   # serve.py sends the .gz
ls -la "$SITE"
