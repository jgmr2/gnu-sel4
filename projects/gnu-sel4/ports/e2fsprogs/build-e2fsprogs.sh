#!/bin/sh
# Builds e2fsprogs (e2fsck, mke2fs, resize2fs, tune2fs, dumpe2fs) for GNU/seL4 with sel4-cc,
# statically. They work on /dev/root, the ext2 partition as a raw device (server/vfs.c): e2fsck
# runs at boot when the disk was not cleanly unmounted (/etc/rc).
#   usage: build-e2fsprogs.sh <source> <build-dir> <sysroot>
# Output: <build-dir>/bin/* (stripped). No /etc/mke2fs.conf: mke2fs then uses its built-in defaults
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

. "$(dirname "$0")/../port-lib.sh"
if port_prepare "$SRC" "$BUILD"; then
# No Linux kernel headers in the sysroot: e2fsprogs includes <linux/...> under __linux__, so it
# is built as a generic Unix (-U__linux__): configure finds no <linux/fs.h> and the library uses
# its portable paths (the device size comes from lseek(SEEK_END))
./configure --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu --prefix=/usr \
    CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2 -U__linux__ -U__linux -Ulinux" BUILD_CC=clang \
    --disable-nls --disable-rpath --disable-fuse2fs --disable-debugfs --disable-imager \
    --disable-defrag --disable-uuidd --disable-e2initrd-helper --disable-tdb \
    --without-libarchive --with-crond-dir=no --with-systemd-unit-dir=no --with-udev-rules-dir=no \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
fi
make -j"$(nproc)" libs >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make -j"$(nproc)" -C e2fsck e2fsck >>make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make -j"$(nproc)" -C misc mke2fs tune2fs dumpe2fs >>make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make -j"$(nproc)" -C resize resize2fs >>make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
mkdir -p bin
for f in e2fsck/e2fsck misc/mke2fs misc/tune2fs misc/dumpe2fs resize/resize2fs; do
    llvm-strip -o "bin/$(basename "$f")" "$f"
done
echo "$(basename "$SRC"): $(ls bin | tr '\n' ' ')"
port_done
