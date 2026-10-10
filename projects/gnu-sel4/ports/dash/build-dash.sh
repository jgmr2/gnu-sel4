#!/bin/sh
# Builds dash for GNU/seL4 with sel4-cc.
#   usage: build-dash.sh <dash-source> <build-dir> <sysroot>
# Output: <build-dir>/src/dash
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)

rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
# Our own signal-name generator (BSD) instead of bash's (GPL)
cp "$HERE/mksignames.c" src/mksignames.c

./configure --host=x86_64-linux-musl --enable-static \
    CC="$SYSROOT/bin/sel4-cc" CC_FOR_BUILD=clang \
    CFLAGS="-O2 -g" >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
ls -la src/dash
