#!/bin/sh
# ./web.sh: boots GNU/seL4 (what ../../dev.sh make built) in the `web` container and serves its
# serial console and screen at http://localhost:6080. disk.img may be a link to another disk:
# its directory is mounted too (DISK_DIR). Extra arguments go to docker compose up.
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
DISK_DIR=$(dirname "$(readlink -f projects/gnu-sel4/disk.img)")
export DISK_DIR
exec docker compose up "$@" web
