#!/bin/sh
# tools/test/smoke.sh: after a change, everything in one command, from the host: the ARM image
# (inside dev.sh), the preflight checks, a fresh test disk, the smoke test. One JSON summary.
# SPDX-License-Identifier: BSD-2-Clause
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); POSIX=$(dirname "$(dirname "$HERE")")
(cd "$POSIX" && ../../dev.sh make PLATFORM=qemu-arm image >/tmp/sel4-image.log 2>&1) ||
    { echo '{"test":"smoke","ok":false,"ev":"image build failed","log":"/tmp/sel4-image.log"}'; exit 1; }
"$HERE/test-disk.sh" >/dev/null
"$HERE/preflight.sh" | grep '"ok":false' && { echo '{"test":"smoke","ok":false,"ev":"preflight"}'; exit 1; }
exec "$HERE/smoke-arm.sh" "$POSIX/build-qemu-arm/test-disk.img"
