#!/bin/sh
# tools/test/test-disk.sh [base.img] [out.img]: the disk tests boot (never a disk in use): the
# scrubbed demo disk with what was just built put in (build-qemu-arm/bin: /sbin for init, 9pd,
# poweroff; passwd and sudo set-user-ID) and rootfs/etc/s6-rc's services; user's shell is dash
# (the first zsh start, its compinit, costs ~20 s a test). JSON line at the end.
# SPDX-License-Identifier: BSD-2-Clause
set -eu
POSIX=$(cd "$(dirname "$0")/../.." && pwd)
BASE=${1:-$POSIX/build-wasm/demo-disk.img}; OUT=${2:-$POSIX/build-qemu-arm/test-disk.img}
DEBUGFS=$(command -v debugfs || echo /sbin/debugfs)
cp "$BASE" "$OUT"
start=$(python3 -c 'import sys; m=open(sys.argv[1],"rb").read(512); print(int.from_bytes(m[454:458],"little"))' "$OUT")
C=$(mktemp); trap 'rm -f "$C" "$C.passwd"' EXIT
n=0
for f in "$POSIX"/build-qemu-arm/bin/*; do
    b=$(basename "$f"); dir=/bin; mode=0100755
    case " init 9pd poweroff " in *" $b "*) dir=/sbin;; esac
    case " passwd sudo " in *" $b "*) mode=0104755;; esac
    printf 'rm %s/%s\nwrite %s %s/%s\nsif %s/%s mode %s\nsif %s/%s uid 0\nsif %s/%s gid 0\n' \
        $dir "$b" "$f" $dir "$b" $dir "$b" $mode $dir "$b" $dir "$b" >> "$C"
    n=$((n + 1))
done
for f in $(cd "$POSIX/rootfs" && find etc/s6-rc/source -type f); do
    printf 'rm /%s\nwrite %s /%s\nsif /%s uid 0\nsif /%s gid 0\n' "$f" "$POSIX/rootfs/$f" "$f" "$f" "$f" >> "$C"
done
sed 's|^\(user:[^:]*:[^:]*:[^:]*:[^:]*:[^:]*:\).*|\1/bin/sh|' "$POSIX/rootfs/etc/passwd" > "$C.passwd"
printf 'rm /etc/passwd\nwrite %s /etc/passwd\nsif /etc/passwd mode 0100644\nsif /etc/passwd uid 0\nsif /etc/passwd gid 0\n' "$C.passwd" >> "$C"
"$DEBUGFS" -w -f "$C" "$OUT?offset=$((start * 512))" >/dev/null 2>&1
echo "{\"c\":\"test-disk\",\"ev\":\"made\",\"disk\":\"$OUT\",\"programs\":$n}"
