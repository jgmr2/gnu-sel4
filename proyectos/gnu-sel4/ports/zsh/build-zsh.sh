#!/bin/sh
# Compila zsh para GNU/seL4 con sel4-cc (estático: los módulos van dentro del binario).
#   uso: build-zsh.sh <fuente-zsh> <dir-de-build> <sysroot>
# Salida: <dir-de-build>/Src/zsh y las funciones en <dir-de-build>/inst/usr/share/zsh
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
# En compilación cruzada configure no puede ejecutar sus pruebas: se le dan las respuestas
# (todas valen para musl + posix_server)
./configure --host=x86_64-linux-musl --build=x86_64-pc-linux-gnu \
    CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2" \
    --prefix=/usr --enable-etcdir=/etc --disable-dynamic --disable-gdbm --disable-pcre --disable-cap \
    --with-tcsetpgrp --with-term-lib="ncursesw" \
    zsh_cv_sys_nis=no zsh_cv_sys_nis_plus=no zsh_cv_shared_environ=yes zsh_cv_sys_elf=yes \
    zsh_cv_sys_dynamic_execsyms=no zsh_cv_getcwd_malloc=yes zsh_cv_rlim_t_is_longer=no \
    zsh_cv_rlim_t_is_quad_t=no zsh_cv_type_rlim_t_is_unsigned=yes \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
# Todos los módulos compilados dentro del binario (no hay carga dinámica)
sed -i 's/^\(.*\)link=dynamic/\1link=static/' config.modules
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make install.bin install.fns DESTDIR="$PWD/inst" >install.log 2>&1 || { tail -20 install.log; exit 1; }
echo "zsh: binario de $(du -h Src/zsh | cut -f1), $(find inst/usr/share/zsh -type f | wc -l) funciones"
