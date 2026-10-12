#!/bin/sh
# Builds the VM's boot image: the Linux kernel and an initramfs with busybox, apk, the kernel's
# modules and our own part (init, the bridge agent). Alpine and Docker go onto the VM's own disk on
# its first boot (overlay/init); out/vm-seed.ext4 is that disk already installed, from the same
# pinned packages, for the tests and new disks (offline).
# It runs INSIDE an alpine container that is only used as a tool, with no network
# (see ../../Makefile, target "vm-rootfs"): everything it installs comes from $PKGS, the files
# pinned in deps.lock (lock.sh) and fetched by fetch-deps.sh, so the image is the same whatever
# day it is built.
#   Output: out/vmlinuz  out/initramfs.cpio.gz  out/vm-seed.ext4
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
PKGS=${PKGS:-/pkgs}
LOCK=${LOCK:-/deps.lock}
# The files deps.lock lists for one set (tools, vm, docker): exactly those, whatever else an
# older lock left in $PKGS
pinned() { awk -v s="/$1" '$1 == "file" && $NF == "vm" && index($2, s) { n = split($2, p, "/"); print p[n-1] "/" p[n] }' "$LOCK" |
           sed "s|^alpine-[^/]*/||; s|^|$PKGS/|"; }
ROOT=/tmp/rootfs
TMP=/tmp/pkgs
OUT=out

rm -rf "$ROOT" "$TMP" && mkdir -p "$ROOT" "$TMP/etc/apk" "$OUT"
cp -r /etc/apk/keys "$TMP/etc/apk/"
for f in $(pinned tools/) $(pinned vm/) $(pinned dockerroot/); do
    [ -f "$f" ] || { echo "missing $f: run ../../fetch-deps.sh" >&2; exit 1; }
done
OFFLINE="--no-network --no-cache --repositories-file /dev/null"

# 1. The pieces: the tools that build the image (compiler, kmod, e2fsprogs) into this container;
#    the linux-virt kernel (binary + modules only), static busybox, static apk, Alpine's keys and
#    the CA certificates into $TMP.
# shellcheck disable=SC2086
apk add $OFFLINE $(pinned tools/) >/dev/null
# shellcheck disable=SC2086
apk add $OFFLINE --root "$TMP" --initdb --no-scripts $(pinned vm/) >/dev/null

# 2. System skeleton
cd "$ROOT"
mkdir -p bin sbin usr/bin usr/sbin etc/ssl/certs etc/apk/keys lib/modules dev proc sys run tmp root var/log
chmod 1777 tmp; chmod 700 root
cd - >/dev/null

# busybox: a single binary that provides sh, init, mount, ls, ip, vi...
cp "$TMP/bin/busybox.static" "$ROOT/bin/busybox"
for app in $("$ROOT/bin/busybox" --list-full); do
    [ -e "$ROOT/$app" ] || ln -s /bin/busybox "$ROOT/$app"
done

# apk (static) and Alpine's keys, to install Alpine and Docker on the VM's disk; the CA
# certificates for its mirror
cp "$TMP/sbin/apk.static" "$ROOT/sbin/"
cp "$TMP"/etc/apk/keys/*.pub "$ROOT/etc/apk/keys/" 2>/dev/null || cp "$TMP"/usr/share/apk/keys/x86_64/*.pub "$ROOT/etc/apk/keys/"
cp "$TMP/etc/ssl/certs/ca-certificates.crt" "$ROOT/etc/ssl/certs/"
ln -sf certs/ca-certificates.crt "$ROOT/etc/ssl/cert.pem"      # where apk's OpenSSL looks
echo "v$(cut -d. -f1,2 /etc/alpine-release)" > "$ROOT/etc/alpine-branch"
cat packages > "$ROOT/etc/vm-packages"

# Agent for the bridge to GNU/seL4 (vm/agent): static binary
mkdir -p "$ROOT/sbin"
gcc -O2 -static -Wall -I../../include -o "$ROOT/sbin/sel4-agent" ../agent/sel4-agent.c

# What the first stage needs for the disk, with its dynamic libraries (musl): mkfs.ext4, e2fsck
# and resize2fs (the seed grows to the partition)
copy_with_libs() {
    for f in "$@"; do
        mkdir -p "$ROOT$(dirname "$f")"
        cp -a "$f" "$ROOT$f"
        [ -L "$f" ] && f=$(readlink -f "$f") && cp -a "$f" "$ROOT$f"
        ldd "$f" 2>/dev/null | awk '/=>/ {print $3; next} $1 ~ /^\// {print $1}' | while read -r lib; do
            [ -e "$ROOT$lib" ] || { mkdir -p "$ROOT$(dirname "$lib")"; cp -L "$lib" "$ROOT$lib"; }
        done
    done
}
copy_with_libs /sbin/mke2fs /sbin/mkfs.ext4 /sbin/e2fsck /usr/sbin/resize2fs

# Kernel and modules (dropping those for hardware a VM does not have)
KVER=$(ls "$TMP/lib/modules")
cp "$TMP/boot/vmlinuz-virt" "$OUT/vmlinuz"
cp -a "$TMP/lib/modules/$KVER" "$ROOT/lib/modules/"
# nor file systems it does not use: the initramfs lives in the VM's RAM for the whole session
for d in drivers/gpu drivers/media drivers/usb drivers/hid drivers/input drivers/infiniband \
         drivers/net/wireless drivers/bluetooth drivers/isdn sound net/bluetooth net/wireless \
         drivers/net/ethernet drivers/net/wan drivers/net/wwan drivers/net/can drivers/net/hamradio \
         drivers/md drivers/scsi drivers/ata drivers/nvme drivers/target drivers/mtd drivers/iio \
         drivers/crypto drivers/net/phy drivers/net/pcs drivers/net/mdio drivers/net/usb \
         arch/x86/kvm net/sunrpc net/ceph net/rds net/tipc net/sctp net/mac80211 net/rxrpc \
         fs/xfs fs/btrfs fs/ocfs2 fs/nfs fs/nfsd fs/smb fs/f2fs fs/ceph fs/gfs2 fs/ntfs3 fs/jfs \
         fs/dlm fs/nilfs2 fs/ubifs fs/bcachefs fs/reiserfs fs/hfs fs/hfsplus fs/udf fs/orangefs \
         fs/afs fs/coda fs/lockd fs/nfs_common fs/minix fs/ufs fs/affs fs/befs fs/omfs fs/efs; do
    rm -rf "$ROOT/lib/modules/$KVER/kernel/$d"
done
# busybox modprobe cannot read compressed modules: decompress them
find "$ROOT/lib/modules" -name "*.ko.gz" -exec gunzip {} +
depmod -b "$ROOT" "$KVER" 2>/dev/null

# 3. Our own part: /init runs here; the rest (inittab, rc, daemon.json, the agent...) goes into
#    the VM's system on every boot (/ours). The DHCP script is needed here too
mkdir -p "$ROOT/ours/sbin"
cp -a overlay/. "$ROOT/ours/"
mv "$ROOT/ours/init" "$ROOT/init"
mv "$ROOT/sbin/sel4-agent" "$ROOT/ours/sbin/"
mkdir -p "$ROOT/usr/share/udhcpc"
cp overlay/usr/share/udhcpc/default.script "$ROOT/usr/share/udhcpc/"

# 4. Pack it as an initramfs, the same bytes on every build: files in a fixed order, one fixed
#    date, GNU cpio's --reproducible (no inode numbers), gzip without a name or a date
find "$ROOT" -exec touch -h -d "@${SOURCE_DATE_EPOCH:-0}" {} +
(cd "$ROOT" && find . | LC_ALL=C sort | cpio -o -H newc --quiet --reproducible) | gzip -n -6 > "$OUT/initramfs.cpio.gz"
# 5. The seed: the VM's disk with Alpine and Docker installed from the pinned packages (offline),
#    as an ext4 image of 1 GiB that the first stage grows to the partition. Not in the boot image
SEED=/tmp/seed
rm -rf "$SEED" && mkdir -p "$SEED/etc/apk/keys"
cp "$ROOT"/etc/apk/keys/*.pub "$SEED/etc/apk/keys/"
# shellcheck disable=SC2046
apk add $OFFLINE --root "$SEED" --initdb $(pinned dockerroot/) >/dev/null
printf 'https://dl-cdn.alpinelinux.org/alpine/%s/main\nhttps://dl-cdn.alpinelinux.org/alpine/%s/community\n' \
    "$(cat "$ROOT/etc/alpine-branch")" "$(cat "$ROOT/etc/alpine-branch")" > "$SEED/etc/apk/repositories"
find "$SEED" -exec touch -h -d "@${SOURCE_DATE_EPOCH:-0}" {} +
rm -f "$OUT/vm-seed.ext4"
E2FSPROGS_FAKE_TIME=${SOURCE_DATE_EPOCH:-0} mke2fs -q -t ext4 -L gnu-sel4-vm -U clear -E hash_seed=00000000-0000-0000-0000-000000000000 \
    -d "$SEED" "$OUT/vm-seed.ext4" 1G
echo "kernel $KVER, $("$SEED/usr/bin/dockerd" --version 2>/dev/null || echo docker in the seed)"
du -sh "$ROOT" "$SEED" "$OUT"/*
