#!/bin/sh
# Builds dash for GNU/seL4 with sel4-cc.
#   usage: build-dash.sh <dash-source> <build-dir> <sysroot>
# Output: <build-dir>/src/dash
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)

. "$(dirname "$0")/../port-lib.sh"
if port_prepare "$SRC" "$BUILD"; then
# Our own signal-name generator (BSD) instead of bash's (GPL)
cp "$HERE/mksignames.c" src/mksignames.c
git apply -p1 "$HERE/ctrl-c-at-prompt.patch"

./configure --host=${SEL4_HOST:-x86_64-linux-musl} --enable-static \
    CC="$SYSROOT/bin/sel4-cc" CC_FOR_BUILD=clang \
    CFLAGS="-O2 -g" >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
fi
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
ls -la src/dash
port_done
