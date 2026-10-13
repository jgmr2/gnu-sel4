#!/bin/sh
# make-snapshot.sh <loader.img> <demo-disk.img> <out-dir>: the browser demo starting at the login
# prompt instead of booting (TODO item 22). A native QEMU built from the same qemu-wasm source
# (the same migration format) boots GNU/seL4 with the page's arguments; at "login:" its state is
# saved (migrate file:...). The page then starts with -incoming. The disk must stay the same.
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
LOADER=$(realpath "$1"); DISKIMG=$(realpath "$2"); OUT=$(realpath "$3")
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(realpath "$(dirname "$0")/../../../../third_party/qemu-wasm")
mkdir -p "$OUT/pack" "$OUT/build-native"
# Our patches to it (tools/wasm/patches), each applied once
for p in "$HERE"/patches/*.patch; do
    git -C "$SRC" apply --reverse --check "$(realpath "$p")" 2>/dev/null || git -C "$SRC" apply "$(realpath "$p")"
done
cp "$LOADER" "$OUT/pack/loader.img"; cp "$DISKIMG" "$OUT/pack/disk.img"
rm -f "$OUT/pack/vm.state" "$OUT/pack/serial.log" "$OUT/pack/mon.sock"
# Just what building QEMU needs (qemu-wasm's own example image also builds a Linux kernel)
docker build -q -t gnu-sel4-qemu-native - >/dev/null <<'DEOF'
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends build-essential ninja-build meson \
        python3 python3-venv pkg-config libglib2.0-dev libpixman-1-dev zlib1g-dev flex bison git \
        ca-certificates && rm -rf /var/lib/apt/lists/*
CMD ["sleep", "infinity"]
DEOF
docker rm -f gnu-sel4-qemu-native >/dev/null 2>&1 || true
docker run --rm -d --user "$(id -u):$(id -g)" --name gnu-sel4-qemu-native -v "$SRC":/qemu/ -v "$OUT/pack":/pack -v "$OUT/build-native":/build-native gnu-sel4-qemu-native >/dev/null
docker exec gnu-sel4-qemu-native sh -c '
set -e
cd /build-native
[ -f Makefile ] || /qemu/configure --static --target-list=aarch64-softmmu --cross-prefix= \
    --without-default-features --enable-system --with-coroutine=ucontext --disable-docs >/dev/null
make -j$(nproc) qemu-system-aarch64 >/dev/null
./qemu-system-aarch64 -M virt,virtualization=on -cpu cortex-a57 -m 2047M -nic none \
    -accel tcg,tb-size=256 -device loader,file=/pack/loader.img,addr=0x70000000,cpu-num=0 \
    -global virtio-mmio.force-legacy=false -drive file=/pack/disk.img,format=raw,if=none,id=hd \
    -device virtio-blk-device,drive=hd,bus=virtio-mmio-bus.1 \
    -serial file:/pack/serial.log -monitor unix:/pack/mon.sock,server,nowait -display none -daemonize
'
python3 - "$OUT/pack" "${WAIT:-login:}" <<'PYEOF'
import os, socket, sys, time
pack = sys.argv[1]; WAIT = sys.argv[2].encode()
t0 = time.time()
while WAIT not in (open(f"{pack}/serial.log", "rb").read() if os.path.exists(f"{pack}/serial.log") else b""):
    if time.time() - t0 > 600: sys.exit("no login prompt in 10 minutes")
    time.sleep(2)
time.sleep(3)
s = socket.socket(socket.AF_UNIX); s.connect(f"{pack}/mon.sock")
s.sendall(b"migrate file:/pack/vm.state\n")
while True:
    time.sleep(2); s.sendall(b"info migrate\n"); time.sleep(1)
    out = s.recv(65536)
    if b"completed" in out: break
    if b"failed" in out: sys.exit("migration failed: " + out.decode(errors="replace"))
s.sendall(b"quit\n")
print("snapshot taken after %d s" % (time.time() - t0))
PYEOF
docker rm -f gnu-sel4-qemu-native >/dev/null
ls -la "$OUT/pack/vm.state"
