#!/usr/bin/env bash
# Usage:
#   ./dev.sh build        build the Docker image (first time only)
#   ./dev.sh              open a shell inside the development environment
#   ./dev.sh <command>    run a command inside the environment, in the current directory
#                         (e.g. from projects/gnu-sel4: ../../dev.sh make)
#
# SPDX-License-Identifier: BSD-2-Clause
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE=sel4-dev

if [ "$1" = "build" ]; then
    exec docker build --target toolchain --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" -t "$IMAGE" "$ROOT"
fi

# Current directory relative to the project root (if it is inside it)
case "$PWD/" in
    "$ROOT"/*) REL="${PWD#"$ROOT"}" ;;
    *)         REL="" ;;
esac

TTY=(-i); [ -t 0 ] && TTY=(-it)
[ $# -eq 0 ] && set -- bash
# nice: a long build leaves the machine usable; ccache keeps its cache next to the downloads
exec docker run --rm "${TTY[@]}" -v "$ROOT":/work -w "/work$REL" -e CCACHE_DIR=/work/.cache/ccache \
    "$IMAGE" nice -n 10 "$@"
