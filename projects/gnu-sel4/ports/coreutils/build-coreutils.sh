#!/bin/sh
# Builds GNU coreutils for GNU/seL4 with sel4-cc, as a single multi-call
# binary (src/coreutils; /bin/ls, /bin/cat... are symlinks to it).
#   usage: build-coreutils.sh <coreutils-source> <build-dir> <sysroot>
# Output: <build-dir>/src/coreutils and <build-dir>/programs.txt (names of the symlinks)
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

. "$(dirname "$0")/../port-lib.sh"
if port_prepare "$SRC" "$BUILD"; then
# stdbuf needs dynamic libraries (LD_PRELOAD): not applicable on GNU/seL4.
# No Linux kernel headers in the sysroot: copy_file_range is declared missing so that
# gnulib uses its portable version (the Linux one checks <linux/version.h>).
./configure --host=x86_64-linux-musl CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2 -g" \
    ac_cv_func_copy_file_range=no gl_cv_host_operating_system=GNU/seL4 \
    --enable-single-binary=symlinks --enable-no-install-program=stdbuf \
    --disable-nls --disable-acl --disable-xattr --disable-libcap --without-selinux \
    --without-libgmp --without-openssl --disable-rpath \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
fi
# SUBDIRS=.: no gnulib-tests nor translations (po), which the programs do not need
make -j"$(nproc)" SUBDIRS=. >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make install SUBDIRS=. DESTDIR="$PWD/inst" >install.log 2>&1 || { tail -20 install.log; exit 1; }
ls inst/usr/local/bin | grep -v '^coreutils$' > programs.txt
echo "coreutils: $(wc -l < programs.txt) programs, $(du -h src/coreutils | cut -f1) binary"
port_done
