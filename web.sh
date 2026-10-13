#!/bin/sh
# ./web.sh: the pages (docker compose up: web and demo). web boots GNU/seL4's ARM64 build (what
# ../../dev.sh make PLATFORM=qemu-arm built) and serves its serial console at http://localhost;
# demo serves the browser demo at http://localhost:8101. disk.img may be a link to another disk:
# its directory is mounted too (DISK_DIR, kept in .env so a plain docker compose up finds it).
# Extra arguments go to docker compose up.
#   ./web.sh x86    the x86_64 build instead (make: serial console and screen)
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
DISK_DIR=$(dirname "$(readlink -f projects/gnu-sel4/disk.img)")
grep -v '^DISK_DIR=' .env 2>/dev/null > .env.new || true
echo "DISK_DIR=$DISK_DIR" >> .env.new && mv .env.new .env
docker rm -f sel4-web-arm >/dev/null 2>&1 || true
if [ "${1:-}" = x86 ]; then
    shift
    export WEB_PLATFORM=qemu WEB_BUILD=build
fi
echo "web: http://localhost   demo: http://localhost:8101"
exec docker compose up "$@"
