#!/bin/sh
# make-demo-disk.sh <arm-disk> <out>: the browser demo's disk (TODO item 22). qemu-wasm loads
# its images whole into the page's memory, so: partition 1 of the ARM build's disk (GNU/seL4's
# ext2) alone, shrunk to its contents plus some room, in an MBR disk of its own. No VM partition.
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
DISK=$1; OUT=$2
ROOM_MB=${ROOM_MB:-48}                      # free space left for the visitor
TMP=$OUT.part
start=$(python3 -c 'import sys; m=open(sys.argv[1],"rb").read(512); print(int.from_bytes(m[454:458],"little"))' "$DISK")
count=$(python3 -c 'import sys; m=open(sys.argv[1],"rb").read(512); print(int.from_bytes(m[458:462],"little"))' "$DISK")
dd if="$DISK" of="$TMP" bs=512 skip="$start" count="$count" conv=sparse status=none
e2fsck -fy "$TMP" >/dev/null 2>&1 || true
resize2fs -M "$TMP" >/dev/null 2>&1
blocks=$(dumpe2fs -h "$TMP" 2>/dev/null | awk -F: '/^Block count/ {print $2+0}')
mb=$(( blocks * 4096 / 1048576 + ROOM_MB ))
resize2fs "$TMP" "${mb}M" >/dev/null 2>&1
truncate -s "$(( (1 + mb) * 1048576 ))" "$OUT"
printf 'label: dos\nstart=2048, size=%sMiB, type=83\n' "$mb" | sfdisk -q "$OUT"
dd if="$TMP" of="$OUT" bs=1M seek=1 conv=notrunc,sparse status=none
rm -f "$TMP"
echo "demo disk: $OUT ($mb MiB of ext2)"
