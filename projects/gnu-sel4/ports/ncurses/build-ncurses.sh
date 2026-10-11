#!/bin/sh
# Builds ncurses for GNU/seL4 with sel4-cc (static library, wide characters)
# and installs it into the sysroot for zsh, vim, etc. to use.
#   usage: build-ncurses.sh <ncurses-source> <build-dir> <sysroot>
# Output: library and headers in the sysroot; programs (clear, tput, tic...) and the
#         terminfo database in <build-dir>/inst
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
# --with-build-cc: the tools ncurses uses while building run on the host
./configure --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
    CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2" --with-build-cc=clang \
    --prefix=/usr --with-terminfo-dirs=/usr/share/terminfo --with-default-terminfo-dir=/usr/share/terminfo \
    --enable-widec --with-normal --without-shared --without-debug --without-cxx --without-cxx-binding \
    --without-ada --without-tests --without-manpages --disable-stripping --enable-pc-files=no \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make install DESTDIR="$PWD/inst" >install.log 2>&1 || { tail -20 install.log; exit 1; }

# Into the sysroot: headers and libraries. With symlinks for the names configure scripts look for
cp -r inst/usr/include/. "$SYSROOT/include/"
cp inst/usr/lib/*.a "$SYSROOT/lib/"
if [ -d "$SYSROOT/include/ncursesw" ]; then
    for h in curses ncurses term termcap; do ln -sf "ncursesw/$h.h" "$SYSROOT/include/$h.h"; done
fi
for l in ncurses curses tinfo termcap; do ln -sf libncursesw.a "$SYSROOT/lib/lib$l.a"; done
echo "ncurses: $(ls inst/usr/bin | tr '\n' ' '); terminfo: $(find inst/usr/share/terminfo -type f | wc -l) terminals"
