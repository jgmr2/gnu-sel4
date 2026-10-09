#!/bin/sh
# Compila procps-ng (ps, top, free, uptime, pgrep, pkill, pidof, watch, vmstat...) para
# GNU/seL4 con sel4-cc, estático. Leen el /proc virtual de posix_server (server/procfs.c).
#   uso: build-procps.sh <fuente-procps> <dir-de-build> <sysroot>
# Salida: <dir-de-build>/inst/usr/local/bin/*
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
# kill ya lo da coreutils; sin NUMA ni systemd. ncursesw (top, watch) está en el sysroot y
# sus banderas van a mano: no hay pkg-config (PKG_CONFIG=true solo pasa la comprobación).
./configure --host=x86_64-linux-musl CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2 -g" \
    --disable-shared --enable-static --disable-nls --disable-rpath --disable-kill \
    --disable-numa --without-systemd --without-elogind --enable-watch8bit \
    ac_cv_func_malloc_0_nonnull=yes ac_cv_func_realloc_0_nonnull=yes \
    NCURSES_CFLAGS="-I$SYSROOT/include" NCURSES_LIBS="-lncursesw" PKG_CONFIG=true \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make install DESTDIR="$PWD/inst" >install.log 2>&1 || { tail -20 install.log; exit 1; }
ls inst/usr/local/bin inst/usr/local/sbin 2>/dev/null
