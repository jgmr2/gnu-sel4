#!/bin/sh
# fetch-deps.sh: downloads every GNU/seL4 dependency listed in deps.lock and checks it
# (SHA-256 for archives, exact commits for git), so a fresh clone can be built.
#   ./fetch-deps.sh              fetch whatever is missing (what is already there is kept)
#   ./fetch-deps.sh --check      only report what is missing or does not match
#   ./fetch-deps.sh --no-vm      leave out the VM image's pieces (entries marked "vm"; the Docker
#                                image does: make vm-rootfs runs on the host)
#   ./fetch-deps.sh --only-vm    only those (docker compose's vm-deps service)
# Needs: sh, curl, tar (xz, bzip2, gzip), git, sha256sum. Downloads are cached in .cache/.
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
ROOT=$(pwd)
CACHE=${CACHE:-$ROOT/.cache/downloads}
CHECK=0 NOVM=0 ONLYVM=0
for a in "$@"; do
    case "$a" in
    --check) CHECK=1 ;; --no-vm) NOVM=1 ;; --only-vm) ONLYVM=1 ;;
    *) echo "usage: $0 [--check] [--no-vm | --only-vm]" >&2; exit 2 ;;
    esac
done
missing=0

say() { printf '%-32s %s\n' "$1" "$2"; }

opt() {         # opt <name> <options...>: value of name=..., or "yes" for a bare flag
    n=$1; shift
    for o in "$@"; do
        case "$o" in "$n") echo yes; return ;; "$n"=*) echo "${o#*=}"; return ;; esac
    done
}

# The same file from other places, tried in order when the first fails: ftp.gnu.org often times
# out from cloud machines (GitHub's runners). The SHA-256 below is what decides, wherever it came from
mirrors() {
    echo "$1"
    case "$1" in https://ftp.gnu.org/gnu/*)
        echo "https://ftpmirror.gnu.org/${1#https://ftp.gnu.org/gnu/}"
        echo "https://mirrors.kernel.org/gnu/${1#https://ftp.gnu.org/gnu/}" ;;
    esac
}

download() {    # download <url> <sha256> -> path of the verified file in the cache
    f="$CACHE/$2-$(basename "$1")"
    if [ ! -s "$f" ]; then
        mkdir -p "$CACHE"
        ok=
        for u in $(mirrors "$1"); do
            if curl -fsSL --retry 3 --connect-timeout 20 --speed-limit 1024 --speed-time 60 -o "$f.part" "$u"; then
                ok=1; break
            fi
            echo "  (failed: $u)" >&2
        done
        [ -n "$ok" ] || {
            echo "download failed: $1" >&2
            case "$1" in */alpine/*) echo "(Alpine keeps only the latest build of each package: 'make vm-lock' in projects/gnu-sel4 pins the current ones)" >&2 ;; esac
            exit 1; }
        mv "$f.part" "$f"
    fi
    if [ "$(sha256sum "$f" | cut -d' ' -f1)" != "$2" ]; then
        rm -f "$f"
        echo "SHA-256 mismatch for $1 (expected $2): refusing to use it" >&2
        exit 1
    fi
    echo "$f"
}

keep_only() {   # keep_only <dir> <a,b,...>: delete everything else in dir (except .git)
    for e in "$1"/* "$1"/.[!.]*; do
        [ -e "$e" ] || continue
        b=$(basename "$e")
        [ "$b" = .git ] && continue
        case ",$2," in *",$b,"*) ;; *) rm -rf "$e" ;; esac
    done
}

fetch_tar() {   # fetch_tar <dest> <url> <sha256> [options]
    dest=$1 url=$2 sha=$3; shift 3
    f=$(download "$url" "$sha")
    tmp=$(mktemp -d "$ROOT/.cache/x.XXXXXX")
    tar -xf "$f" -C "$tmp"
    set -- "$tmp"/*
    [ $# -eq 1 ] && [ -d "$1" ] || { echo "$url: expected one top directory" >&2; exit 1; }
    only=$(opt only "$@")
    [ -n "$only" ] && keep_only "$1" "$only"
    mkdir -p "$(dirname "$dest")"
    mv "$1" "$dest"
    rmdir "$tmp"
}

fetch_git() {   # fetch_git <dest> <url> <commit> [options]
    dest=$1 url=$2 commit=$3; shift 3
    git init -q "$dest"
    git -C "$dest" remote add origin "$url"
    git -C "$dest" fetch -q --depth 1 origin "$commit"
    git -C "$dest" checkout -q FETCH_HEAD
    [ "$(opt submodules "$@")" = yes ] && git -C "$dest" submodule -q update --init --depth 1
    only=$(opt only "$@")
    [ -n "$only" ] && keep_only "$dest" "$only"
    patches=$(opt patches "$@")
    if [ -n "$patches" ]; then
        git -C "$dest" checkout -q -b gnu-sel4
        GIT_COMMITTER_NAME=fetch-deps GIT_COMMITTER_EMAIL=fetch-deps@gnu-sel4 \
            git -C "$dest" am -q "$ROOT/$patches"/*.patch
    fi
}

present_ok() {  # present_ok <kind> <dest> <ref> [options]: is what is on disk the right thing?
    kind=$1 dest=$2 ref=$3; shift 3
    case "$kind" in
    git)
        [ -d "$dest/.git" ] || return 1
        base=$(git -C "$dest" rev-parse HEAD)
        [ -n "$(opt patches "$@")" ] && base=$(git -C "$dest" merge-base HEAD "$ref" 2>/dev/null || echo none)
        [ "$base" = "$ref" ] ;;
    *)  [ -e "$dest" ] ;;
    esac
}

mkdir -p "$ROOT/.cache"
grep -v '^\s*#' deps.lock | grep -v '^\s*$' | while read -r kind dest src ref rest; do
    # shellcheck disable=SC2086
    set -- $rest
    [ $NOVM = 1 ] && [ "$(opt vm "$@")" = yes ] && continue
    [ $ONLYVM = 1 ] && [ "$(opt vm "$@")" != yes ] && continue
    if [ "$kind" = image ]; then say "$dest" "image $src@$ref (docker pulls it)"; continue; fi
    if [ -e "$dest" ]; then
        if present_ok "$kind" "$dest" "$ref" "$@"; then say "$dest" ok; else say "$dest" "DIFFERS from deps.lock"; fi
        continue
    fi
    if [ $CHECK = 1 ]; then say "$dest" MISSING; continue; fi
    say "$dest" fetching...
    case "$kind" in
    tar)  fetch_tar "$dest" "$src" "$ref" "$@" ;;
    file) f=$(download "$src" "$ref"); mkdir -p "$(dirname "$dest")"; cp "$f" "$dest" ;;
    git)  fetch_git "$dest" "$src" "$ref" "$@" ;;
    *)    echo "deps.lock: unknown kind '$kind'" >&2; exit 1 ;;
    esac
done
