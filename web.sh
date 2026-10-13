#!/bin/sh
# ./web.sh: boots GNU/seL4 (what ../../dev.sh make built) in the `web` container and serves its
# serial console and screen at http://localhost (port 80). disk.img may be a link to another disk:
# its directory is mounted too (DISK_DIR). Extra arguments go to docker compose up.
#   ./web.sh arm    the ARM64 build instead (make PLATFORM=qemu-arm: serial console only)
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
DISK_DIR=$(dirname "$(readlink -f projects/gnu-sel4/disk.img)")
export DISK_DIR
if [ "${1:-}" = arm ]; then
    shift
    docker rm -f sel4-web-arm >/dev/null 2>&1 || true
    docker compose stop web >/dev/null 2>&1 || true
    echo "web (ARM64): http://localhost"
    exec docker compose run -d --name sel4-web-arm --service-ports -e PLATFORM=qemu-arm \
        -e BUILD_DIR=/work/projects/gnu-sel4/build-qemu-arm "$@" web
fi
docker rm -f sel4-web-arm >/dev/null 2>&1 || true
exec docker compose up "$@" web
