#!/bin/sh
# Compila ncurses para GNU/seL4 con sel4-cc (biblioteca estática, caracteres anchos)
# y la instala en el sysroot para que la usen zsh, vim, etc.
#   uso: build-ncurses.sh <fuente-ncurses> <dir-de-build> <sysroot>
# Salida: biblioteca y headers en el sysroot; programas (clear, tput, tic...) y la base
#         terminfo en <dir-de-build>/inst
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
# --with-build-cc: las herramientas que ncurses usa al compilarse corren en el host
./configure --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
    CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2" --with-build-cc=clang \
    --prefix=/usr --with-terminfo-dirs=/usr/share/terminfo --with-default-terminfo-dir=/usr/share/terminfo \
    --enable-widec --with-normal --without-shared --without-debug --without-cxx --without-cxx-binding \
    --without-ada --without-tests --without-manpages --disable-stripping --enable-pc-files=no \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make install DESTDIR="$PWD/inst" >install.log 2>&1 || { tail -20 install.log; exit 1; }

# Al sysroot: headers y bibliotecas. Con enlaces para los nombres que buscan los configure
cp -r inst/usr/include/. "$SYSROOT/include/"
cp inst/usr/lib/*.a "$SYSROOT/lib/"
if [ -d "$SYSROOT/include/ncursesw" ]; then
    for h in curses ncurses term termcap; do ln -sf "ncursesw/$h.h" "$SYSROOT/include/$h.h"; done
fi
for l in ncurses curses tinfo termcap; do ln -sf libncursesw.a "$SYSROOT/lib/lib$l.a"; done
echo "ncurses: $(ls inst/usr/bin | tr '\n' ' '); terminfo: $(find inst/usr/share/terminfo -type f | wc -l) terminales"
