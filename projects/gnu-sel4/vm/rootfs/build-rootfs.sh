#!/bin/sh
# Builds a minimal "distro": Linux kernel + busybox + Docker. Nothing else.
# It runs INSIDE an alpine container that is only used as a tool
# (see ../../Makefile, target "vm-rootfs").
#   Salida: out/vmlinuz  out/initramfs.cpio.gz
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
DOCKER_VERSION=29.9.0
ROOT=/tmp/rootfs
TMP=/tmp/pkgs
OUT=out

rm -rf "$ROOT" "$TMP" && mkdir -p "$ROOT" "$TMP/etc/apk" "$OUT"
cp -r /etc/apk/keys /etc/apk/repositories "$TMP/etc/apk/"

# 1. Fetch the pieces: linux-virt kernel (binary + modules only), static busybox,
#    CA certificates and Docker's official static binaries.
apk add --no-cache curl kmod >/dev/null
apk add --root "$TMP" --initdb --no-cache --no-scripts linux-virt busybox-static ca-certificates-bundle >/dev/null
curl -fsSL "https://download.docker.com/linux/static/stable/x86_64/docker-$DOCKER_VERSION.tgz" | tar -xz -C "$TMP"

# 2. System skeleton
cd "$ROOT"
mkdir -p bin sbin usr/bin usr/sbin etc/ssl/certs lib/modules dev proc sys run tmp root var/log var/lib/docker
chmod 1777 tmp; chmod 700 root
cd - >/dev/null

# busybox: a single binary that provides sh, init, mount, ls, ip, vi...
cp "$TMP/bin/busybox.static" "$ROOT/bin/busybox"
for app in $("$ROOT/bin/busybox" --list-full); do
    [ -e "$ROOT/$app" ] || ln -s /bin/busybox "$ROOT/$app"
done

# Docker (static binaries: dockerd, containerd, runc, docker, ...)
cp "$TMP"/docker/* "$ROOT/usr/bin/"
cp "$TMP/etc/ssl/certs/ca-certificates.crt" "$ROOT/etc/ssl/certs/"

# Agent for the bridge to GNU/seL4 (vm/agent): static binary
apk add --no-cache build-base linux-headers >/dev/null
mkdir -p "$ROOT/sbin"
gcc -O2 -static -Wall -I../../include -o "$ROOT/sbin/sel4-agent" ../agent/sel4-agent.c

# Tools Docker and the boot need, with their dynamic libraries (musl):
#   iptables (NAT rules for Docker's networks) and mkfs.ext4/e2fsck (persistent disk)
apk add --no-cache iptables e2fsprogs >/dev/null
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
copy_with_libs /usr/sbin/xtables-nft-multi /sbin/mke2fs /sbin/mkfs.ext4 /sbin/e2fsck
copy_with_libs /usr/lib/xtables/*.so
for t in iptables ip6tables iptables-save iptables-restore ip6tables-save ip6tables-restore; do
    ln -sf xtables-nft-multi "$ROOT/usr/sbin/$t"
done

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
# ctr: containerd's debugging client; Docker does not use it
rm -f "$ROOT/usr/bin/ctr"
# busybox modprobe cannot read compressed modules: decompress them
find "$ROOT/lib/modules" -name "*.ko.gz" -exec gunzip {} +
depmod -b "$ROOT" "$KVER" 2>/dev/null

# 3. Our own configuration (init, inittab, rc, daemon.json...)
cp -a overlay/. "$ROOT/"

# 4. Pack it as an initramfs
(cd "$ROOT" && find . | cpio -o -H newc --quiet) | gzip -6 > "$OUT/initramfs.cpio.gz"
echo "kernel $KVER, docker $DOCKER_VERSION"
du -sh "$ROOT" "$OUT"/*
