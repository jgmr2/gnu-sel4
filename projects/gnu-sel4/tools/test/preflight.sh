#!/bin/sh
# tools/test/preflight.sh: what to check before a test (docs/kb: stale images, leftover containers,
# a disk in use); JSON lines, exit 1 if something is wrong. Leftover test containers are removed.
# SPDX-License-Identifier: BSD-2-Clause
POSIX=$(cd "$(dirname "$0")/../.." && pwd); B=$POSIX/build-qemu-arm; bad=0
say() { echo "{\"c\":\"preflight\",\"ev\":\"$1\",\"ok\":$2${3:+,\"detail\":\"$3\"}}"; [ "$2" = true ] || bad=1; }
left=$(docker ps -aq --filter name=sel4-test- | wc -l)
[ "$left" -gt 0 ] && docker rm -f $(docker ps -aq --filter name=sel4-test-) >/dev/null
say leftover-containers-removed true "$left"
docker image inspect gnu-sel4-web:local >/dev/null 2>&1 && say qemu-image true || say qemu-image false "docker compose build web"
[ -f "$B/loader.img" ] || { say image false "make PLATFORM=qemu-arm image"; exit 1; }
newer=$(find "$POSIX/server" "$POSIX/fs" "$POSIX/meta.py" "$POSIX/system.mk" -newer "$B/loader.img" -type f | head -3 | tr '\n' ' ')
[ -z "$newer" ] && say image-fresh true || say image-fresh false "older than: $newer"
apps=$(for d in "$POSIX"/apps/*/; do b=$(basename "$d"); [ -f "$B/bin/$b" ] && find "$d" -newer "$B/bin/$b" -type f | head -1; done | tr '\n' ' ')
[ -z "$apps" ] && say apps-fresh true || say apps-fresh false "rebuild: $apps"
[ -f "$B/test-disk.img" ] && [ -z "$(find "$B/bin" "$POSIX/rootfs/etc" -newer "$B/test-disk.img" -type f | head -1)" ] \
    && say test-disk-fresh true || say test-disk-fresh false "tools/test/test-disk.sh"
exit $bad
