#!/bin/sh
# socat (serves as nc and much more) for GNU/seL4: no OpenSSL, readline or libwrap.
#   usage: build-socat.sh <socat-source> <build-dir> <sysroot>     Output: <dir>/socat
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)
rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
./configure --host=x86_64-linux-musl CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2" \
    --disable-openssl --disable-readline --disable-libwrap --disable-fips \
    sc_cv_termios_ispeed=no sc_cv_sys_crdly_shift=9 sc_cv_sys_tabdly_shift=11 sc_cv_sys_csize_shift=4 \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"$(nproc)" socat >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
echo "socat: $(du -h socat | cut -f1)"
