#!/bin/sh
# Builds zsh for GNU/seL4 with sel4-cc (static: the modules go inside the binary).
#   usage: build-zsh.sh <zsh-source> <build-dir> <sysroot>
# Output: <build-dir>/Src/zsh and the functions in <build-dir>/inst/usr/share/zsh
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
# When cross-compiling configure cannot run its checks: it is given the answers
# (all valid for musl + posix_server)
./configure --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
    CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2" \
    --prefix=/usr --enable-etcdir=/etc --disable-dynamic --disable-gdbm --disable-pcre --disable-cap \
    --with-tcsetpgrp --with-term-lib="ncursesw" \
    zsh_cv_sys_nis=no zsh_cv_sys_nis_plus=no zsh_cv_shared_environ=yes zsh_cv_sys_elf=yes \
    zsh_cv_sys_dynamic_execsyms=no zsh_cv_getcwd_malloc=yes zsh_cv_rlim_t_is_longer=no \
    zsh_cv_rlim_t_is_quad_t=no zsh_cv_type_rlim_t_is_unsigned=yes \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
# Every module built into the binary (there is no dynamic loading)
sed -i 's/^\(.*\)link=dynamic/\1link=static/' config.modules
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make install.bin install.fns DESTDIR="$PWD/inst" >install.log 2>&1 || { tail -20 install.log; exit 1; }
echo "zsh: $(du -h Src/zsh | cut -f1) binary, $(find inst/usr/share/zsh -type f | wc -l) functions"
