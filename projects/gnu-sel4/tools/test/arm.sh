#!/bin/sh
# tools/test/arm.sh <disk.img> <step>...: one bounded test of the ARM64 build (make PLATFORM=
# qemu-arm image) in QEMU, on a fresh copy of the disk (the original is never written), with
# console.py's steps and logs. The QEMU is the web image's; its container is removed at the end,
# whatever happens. Environment: TEST (its name), TIMEOUT (seconds a step may take).
# SPDX-License-Identifier: BSD-2-Clause
set -eu
POSIX=$(cd "$(dirname "$0")/../.." && pwd)
DISK=$(realpath "$1"); shift
W=$(mktemp -d "${TMPDIR:-/tmp}/sel4-test.XXXXXX")
NAME=sel4-test-$$
clean() { docker rm -f "$NAME" >/dev/null 2>&1; rm -rf "$W" 2>/dev/null; true; }
trap 'clean; exit 1' INT TERM
cp "$DISK" "$W/disk.img"; cp "$POSIX/build-qemu-arm/loader.img" "$W/"
docker run --rm -d --user "$(id -u):$(id -g)" --name "$NAME" --entrypoint qemu-system-aarch64 -v "$W":/t "${QEMU_IMAGE:-gnu-sel4-web:local}" \
    -machine virt,virtualization=on -cpu cortex-a57 -m 2G -device loader,file=/t/loader.img,addr=0x70000000,cpu-num=0 \
    -display none -global virtio-mmio.force-legacy=false -drive file=/t/disk.img,format=raw,if=none,id=hd \
    -device virtio-blk-device,drive=hd,bus=virtio-mmio-bus.1 \
    -netdev user,id=n -device virtio-net-device,netdev=n,bus=virtio-mmio-bus.0 -serial unix:/t/ser.sock,server,nowait -monitor none >/dev/null
python3 "$POSIX/tools/test/console.py" "$W/ser.sock" "$@" && r=0 || r=$?
clean
exit $r      # the test's own result (an EXIT trap's last command would replace it under dash)
