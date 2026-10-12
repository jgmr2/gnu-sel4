#!/bin/sh
# fs-bench.sh [dir]: 4 x 256 MiB written and synced (1 GiB), then 256 MiB read (tools/fs-bench.py runs it
# on GNU/seL4 and on a plain Linux guest under the same QEMU). Times from /proc/uptime (busybox date has no %N)
#
# SPDX-License-Identifier: BSD-2-Clause
now() { read u _ < /proc/uptime; echo $(( ${u%.*} * 1000 + 1${u#*.}0 - 1000 )); }
ms() { echo $(( $(now) - $1 )); }
dir=${1:-/tmp}
# GNU dd (GNU/seL4: its reads of /dev/zero are 64 KiB at a time) needs whole blocks; busybox's has no --version
FB=; dd --version >/dev/null 2>&1 && FB=iflag=fullblock
if [ -n "$BIG" ]; then        # BIG=N (fs-bench.py --gib N): one N GiB file, written, synced, read back
    t=$(now); dd if=/dev/zero of=$dir/big bs=1M count=$((BIG * 1024)) $FB 2>/dev/null; sync; w=$(ms $t)
    [ -w /proc/sys/vm/drop_caches ] && echo 3 > /proc/sys/vm/drop_caches
    t=$(now); cat $dir/big > /dev/null; r=$(ms $t)
    ls -l $dir/big | sed 's/^/BENCH-FILE /'
    rm $dir/big; sync
    echo "BENCH write $BIG GiB: $w ms ($((BIG * 1024 * 1000 / w)) MB/s)  read $BIG GiB: $r ms ($((BIG * 1024 * 1000 / r)) MB/s)"
    exit 0
fi
t=$(now)
for i in 1 2 3 4; do dd if=/dev/zero of=$dir/big bs=1M count=256 $FB 2>/dev/null; sync; [ $i -lt 4 ] && rm $dir/big && sync; done
w=$(ms $t)
[ -w /proc/sys/vm/drop_caches ] && echo 3 > /proc/sys/vm/drop_caches
t=$(now); cat $dir/big > /dev/null; r=$(ms $t)
rm $dir/big; sync
echo "BENCH write 1 GiB: $w ms ($((1024 * 1000 / w)) MB/s)  read 256 MiB: $r ms ($((256 * 1000 / r)) MB/s)"
