#!/bin/sh
# Builds vim for GNU/seL4 with sel4-cc (text mode, with ncurses).
#   usage: build-vim.sh <vim-source> <build-dir> <sysroot>
# Output: <build-dir>/src/vim and a trimmed runtime in <build-dir>/runtime-min
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

. "$(dirname "$0")/../port-lib.sh"
if port_prepare "$SRC" "$BUILD" "$SYSROOT/lib/libncursesw.a"; then
# Answers to the checks configure cannot run when cross-compiling.
# timer_create with SIGEV_THREAD needs threads: vim will use setitimer.
./configure --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
    CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2" \
    --prefix=/usr --with-features=normal --with-tlib=ncursesw \
    --disable-gui --without-x --disable-netbeans --disable-channel --disable-terminal \
    --disable-nls --disable-gpm --disable-sysmouse --disable-libsodium --disable-canberra \
    --disable-selinux --disable-acl --disable-xattr --disable-socketserver \
    vim_cv_toupper_broken=no vim_cv_terminfo=yes vim_cv_tgetent=zero vim_cv_getcwd_broken=no \
    vim_cv_stat_ignores_slash=no vim_cv_memmove_handles_overlap=yes \
    vim_cv_tty_group=world vim_cv_tty_mode=0620 \
    vim_cv_timer_create=no vim_cv_timer_create_with_lrt=no \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
fi
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }

# Trimmed runtime (the full one takes ~40 MB): what is needed for highlighting and common file types
R=runtime; M=runtime-min
rm -rf "$M"; mkdir -p "$M/syntax" "$M/ftplugin" "$M/indent" "$M/colors" "$M/autoload"
cp "$R"/defaults.vim "$R"/filetype.vim "$R"/ftplugin.vim "$R"/indent.vim "$R"/ftoff.vim \
   "$R"/ftplugof.vim "$R"/indoff.vim "$R"/scripts.vim "$M"/ 2>/dev/null || true
cp -r "$R"/autoload/dist "$M/autoload/" 2>/dev/null || true
for f in syntax nosyntax synload syncolor manual c cpp sh zsh vim python make markdown html css javascript \
         json yaml conf diff dockerfile asm help; do
    cp "$R/syntax/$f.vim" "$M/syntax/" 2>/dev/null || true
done
# some languages' ftplugins load their "compiler" (:make)
mkdir -p "$M/compiler"
for f in c sh zsh vim python make markdown; do
    cp "$R/ftplugin/$f.vim" "$M/ftplugin/" 2>/dev/null || true
    cp "$R/indent/$f.vim" "$M/indent/" 2>/dev/null || true
    cp "$R/compiler/$f.vim" "$M/compiler/" 2>/dev/null || true
done
cp "$R/compiler/gcc.vim" "$R/compiler/shellcheck.vim" "$M/compiler/" 2>/dev/null || true
cp "$R"/colors/*.vim "$M/colors/" 2>/dev/null || true
cp -r "$R"/colors/lists "$M/colors/"                 # colour names (syncolor.vim)
# netrw, the file explorer (:Ex, :e ., vim dir/), an optional package since 9.1; and matchparen
mkdir -p "$M/plugin" "$M/pack/dist/opt"
cp "$R"/plugin/netrwPlugin.vim "$R"/plugin/matchparen.vim "$M/plugin/"
cp -r "$R"/pack/dist/opt/netrw "$M/pack/dist/opt/" && rm -rf "$M/pack/dist/opt/netrw/doc"
echo "vim: $(du -h src/vim | cut -f1) binary, $(du -sh $M | cut -f1) trimmed runtime"
port_done
