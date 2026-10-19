#!/bin/sh
# tools/test/smoke.sh: after a change, everything in one command, from the host: the whole ARM
# build from scratch (inside dev.sh: no stale objects or flags hide an error) onto a brand-new
# disk (never the demo disk-arm.img), a test disk from it, the preflight checks, the smoke test.
# One JSON summary. The build's log: /tmp/sel4-build.log.
# SPDX-License-Identifier: BSD-2-Clause
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); POSIX=$(dirname "$(dirname "$HERE")")
rm -rf "$POSIX/build-qemu-arm"
echo '{"c":"smoke","ev":"full build","log":"/tmp/sel4-build.log"}'
(cd "$POSIX" && ../../dev.sh make PLATFORM=qemu-arm 'DISK=$(BUILD_DIR)/fresh-disk.img' >/tmp/sel4-build.log 2>&1) ||
    { echo '{"test":"smoke","ok":false,"ev":"build failed","log":"/tmp/sel4-build.log"}'; exit 1; }
"$HERE/test-disk.sh"
"$HERE/preflight.sh" | grep '"ok":false' && { echo '{"test":"smoke","ok":false,"ev":"preflight"}'; exit 1; }
exec "$HERE/smoke-arm.sh" "$POSIX/build-qemu-arm/test-disk.img"
