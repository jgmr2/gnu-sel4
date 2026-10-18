#!/bin/sh
# tools/test/test-disk.sh [base.img] [out.img]: the disk tests boot (never a disk in use): the
# scrubbed demo disk with everything the last build installed put in (its install.cmds: a disk
# with only some programs new mixes runtimes: docs/kb "stale-test-disk"); user's shell is dash
# (the first zsh start, its compinit, costs ~20 s a test). JSON line at the end.
# SPDX-License-Identifier: BSD-2-Clause
set -eu
POSIX=$(cd "$(dirname "$0")/../.." && pwd)
BASE=${1:-$POSIX/build-wasm/demo-disk.img}; OUT=${2:-$POSIX/build-qemu-arm/test-disk.img}
DEBUGFS=$(command -v debugfs || echo /sbin/debugfs)
cp "$BASE" "$OUT"
start=$(python3 -c 'import sys; m=open(sys.argv[1],"rb").read(512); print(int.from_bytes(m[454:458],"little"))' "$OUT")
C=$(mktemp); trap 'rm -f "$C" "$C.passwd"' EXIT
# Exactly what the last `make PLATFORM=qemu-arm` installed: its debugfs commands (install.cmds,
# paths relative to the build directory, or under /work: the repository inside dev.sh)
REPO=$(cd "$POSIX/../.." && pwd)
[ -f "$POSIX/build-qemu-arm/install.cmds" ] || { echo '{"c":"test-disk","ev":"no install.cmds: make PLATFORM=qemu-arm first"}'; exit 1; }
sed "s|/work/|$REPO/|g" "$POSIX/build-qemu-arm/install.cmds" > "$C"
n=$(grep -c '^write' "$C")
sed 's|^\(user:[^:]*:[^:]*:[^:]*:[^:]*:[^:]*:\).*|\1/bin/sh|' "$POSIX/rootfs/etc/passwd" > "$C.passwd"
printf 'rm /etc/passwd\nwrite %s /etc/passwd\nsif /etc/passwd mode 0100644\nsif /etc/passwd uid 0\nsif /etc/passwd gid 0\n' "$C.passwd" >> "$C"
(cd "$POSIX/build-qemu-arm" && "$DEBUGFS" -w -f "$C" "$OUT?offset=$((start * 512))" >/dev/null 2>&1)
echo "{\"c\":\"test-disk\",\"ev\":\"made\",\"disk\":\"$OUT\",\"programs\":$n}"
