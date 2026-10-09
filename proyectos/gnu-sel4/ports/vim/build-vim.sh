#!/bin/sh
# Compila vim para GNU/seL4 con sel4-cc (modo texto, con ncurses).
#   uso: build-vim.sh <fuente-vim> <dir-de-build> <sysroot>
# Salida: <dir-de-build>/src/vim y un runtime reducido en <dir-de-build>/runtime-min
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
# Respuestas a las pruebas que configure no puede ejecutar en compilación cruzada.
# timer_create con SIGEV_THREAD necesita hilos: vim usará setitimer.
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
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }

# Runtime reducido (el completo ocupa ~40 MB): lo necesario para resaltado y tipos de archivo comunes
R=runtime; M=runtime-min
rm -rf "$M"; mkdir -p "$M/syntax" "$M/ftplugin" "$M/indent" "$M/colors" "$M/autoload"
cp "$R"/defaults.vim "$R"/filetype.vim "$R"/ftplugin.vim "$R"/indent.vim "$R"/ftoff.vim \
   "$R"/ftplugof.vim "$R"/indoff.vim "$R"/scripts.vim "$M"/ 2>/dev/null || true
cp -r "$R"/autoload/dist "$M/autoload/" 2>/dev/null || true
for f in syntax nosyntax synload syncolor manual c cpp sh zsh vim python make markdown html css javascript \
         json yaml conf diff dockerfile asm help; do
    cp "$R/syntax/$f.vim" "$M/syntax/" 2>/dev/null || true
done
# los ftplugin de algunos lenguajes cargan su "compiler" (:make)
mkdir -p "$M/compiler"
for f in c sh zsh vim python make markdown; do
    cp "$R/ftplugin/$f.vim" "$M/ftplugin/" 2>/dev/null || true
    cp "$R/indent/$f.vim" "$M/indent/" 2>/dev/null || true
    cp "$R/compiler/$f.vim" "$M/compiler/" 2>/dev/null || true
done
cp "$R/compiler/gcc.vim" "$R/compiler/shellcheck.vim" "$M/compiler/" 2>/dev/null || true
cp "$R"/colors/*.vim "$M/colors/" 2>/dev/null || true
cp -r "$R"/colors/lists "$M/colors/"                 # nombres de colores (syncolor.vim)
echo "vim: binario de $(du -h src/vim | cut -f1), runtime reducido de $(du -sh $M | cut -f1)"
