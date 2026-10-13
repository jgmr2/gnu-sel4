#!/bin/sh
# make-site.sh <qemu-wasm-dir> <loader.img> <demo-disk.img> <site-dir>: the browser demo's files
# (TODO item 22): qemu-wasm, the page, and the images packed with emscripten's file packager.
# Serve <site-dir> with serve.py (COOP/COEP headers: SharedArrayBuffer needs them).
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
QW=$(realpath "$1"); LOADER=$(realpath "$2"); DISKIMG=$(realpath "$3"); SITE=$4
HERE=$(cd "$(dirname "$0")" && pwd)
rm -rf "$SITE" && mkdir -p "$SITE/pack"
cp "$HERE/site/index.html" "$HERE/site/module.js" "$SITE/"
cp "$QW/qemu-system-aarch64" "$SITE/qemu-system-aarch64.js"
# The shared memory's maximum: 65535 pages, what the TCG backend's per-block modules declare
# (emscripten writes 65536 for 4 GiB and those modules then fail to link)
sed -i 's|"maximum": 4294967296 / 65536,|"maximum": 4294901760 / 65536,|' "$SITE/qemu-system-aarch64.js"
# addFunction, which the TCG backend calls (tcg/wasm32.c) and emscripten 3.1.50 leaves out: each
# compiled block's function goes into a new table entry
python3 - "$SITE/qemu-system-aarch64.js" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
add = "var addFunction = (func, sig) => { var idx = wasmTable.length; wasmTable.grow(1); wasmTable.set(idx, func); return idx; };\n"
if "var addFunction" not in s:
    s = s.replace("\nvar wasmTable;\n", "\nvar wasmTable;\n" + add, 1)
open(p, "w").write(s)
PYEOF
cp "$QW/qemu-system-aarch64.wasm" "$QW/qemu-system-aarch64.worker.js" "$SITE/"
cp "$LOADER" "$SITE/pack/loader.img"; cp "$DISKIMG" "$SITE/pack/disk.img"
SITE=$(realpath "$SITE")
docker run --rm -v "$SITE":/site -w /site gnu-sel4-qemu-wasm-build \
    sh -c '/emsdk/upstream/emscripten/tools/file_packager.py qemu-system-aarch64.data --preload /site/pack@/pack > load.js'
rm -rf "$SITE/pack"
ls -la "$SITE"
