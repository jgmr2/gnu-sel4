#!/bin/sh
# Pins the VM image (make vm-lock, on the host): resolves today's Alpine packages and Docker's
# static binaries once, and writes them, each with its URL and SHA-256, into the "VM image"
# section of deps.lock. From then on build-rootfs.sh only installs those files, offline, so the
# VM is the same whatever day it is built. Run it again to move to newer packages (Alpine keeps
# only the latest build of each package on its mirrors: when one disappears, fetch-deps.sh
# fails and says so, and this is the way forward).
#
#   lock.sh            on the host: pulls the base image, runs itself inside it, edits deps.lock
#   lock.sh --inside   inside the container: prints the deps.lock lines
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
ALPINE=alpine:3.24
# What build-rootfs.sh installs: the tools that build the image (in the container), the
# packages whose files go into the VM's boot image (its kernel, and apk to install the rest), and
# the Alpine system with Docker that the VM installs on its own disk on its first boot: pinned
# here only for the seed that tests and new disks start from (a real install takes what Alpine
# has that day)
TOOLS="build-base linux-headers kmod iptables e2fsprogs e2fsprogs-extra cpio"
VM="linux-virt busybox-static ca-certificates-bundle apk-tools-static alpine-keys"
DOCKERROOT=${DOCKERROOT:-$(cat "$(dirname "$0")/packages" 2>/dev/null)}   # vm/rootfs/packages

if [ "${1:-}" = --inside ]; then
    rel=$(cut -d. -f1,2 /etc/alpine-release)
    apk update -q
    for set in tools vm dockerroot; do
        # tools also takes every package of the base image: a newer library among the tools'
        # dependencies must not leave the base's own users of the old one behind
        case $set in
        tools) pkgs="$TOOLS $(apk info -q | tr '\n' ' ')" ;;
        vm) pkgs=$VM ;;
        dockerroot) pkgs=$DOCKERROOT ;;
        esac
        mkdir -p "/tmp/lock/$set"
        # shellcheck disable=SC2086
        apk fetch --recursive --url -o "/tmp/lock/$set" $pkgs | grep '^https://' | sort |
        while read -r url; do
            f=$(basename "$url")
            echo "file third_party/alpine-$rel/$set/$f $url $(sha256sum "/tmp/lock/$set/$f" | cut -d' ' -f1) vm"
        done
    done
    exit 0
fi

cd "$(dirname "$0")"
LOCK=$(cd ../../../.. && pwd)/deps.lock
docker pull -q "$ALPINE" >/dev/null
digest=$(docker image inspect "$ALPINE" --format '{{index .RepoDigests 0}}')
new=$(mktemp)
{
    echo "image alpine $ALPINE ${digest#*@} vm"
    docker run --rm -e DOCKERROOT="$(cat packages)" -v "$(pwd)/lock.sh:/lock.sh:ro" "$ALPINE@${digest#*@}" sh /lock.sh --inside
} > "$new"
begin='# >>> VM image'
end='# <<< VM image'
grep -q "^$begin" "$LOCK" || { echo "deps.lock: no '$begin' section" >&2; exit 1; }
awk -v f="$new" -v b="$begin" -v e="$end" '
    index($0, b) == 1 { print; while ((getline l < f) > 0) print l; skip = 1; next }
    index($0, e) == 1 { skip = 0 }
    !skip' "$LOCK" > "$LOCK.new"
mv "$LOCK.new" "$LOCK"
# docker compose's vm-rootfs service names the same base image
sed -i -E -e "s|alpine:[0-9.]+@sha256:[0-9a-f]+|$ALPINE@${digest#*@}|" -e "s|third_party/alpine-[0-9.]+|third_party/alpine-${ALPINE#alpine:}|" \
    "$(dirname "$LOCK")/docker-compose.yml"
rm -f "$new"
echo "deps.lock: VM image pinned ($ALPINE ${digest#*@}, $(grep -c ' vm$' "$LOCK") entries)"
