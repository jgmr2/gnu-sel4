#!/bin/sh
# Compila GNU coreutils para GNU/seL4 con sel4-cc, como un solo binario
# multi-llamada (src/coreutils; /bin/ls, /bin/cat... son enlaces a él).
#   uso: build-coreutils.sh <fuente-coreutils> <dir-de-build> <sysroot>
# Salida: <dir-de-build>/src/coreutils y <dir-de-build>/programas.txt (nombres de los enlaces)
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)

rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
# stdbuf necesita bibliotecas dinámicas (LD_PRELOAD): no aplica en GNU/seL4.
# Sin headers del kernel Linux en el sysroot: copy_file_range se declara ausente para que
# gnulib use su versión portable (la de Linux consulta <linux/version.h>).
./configure --host=x86_64-linux-musl CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2 -g" \
    ac_cv_func_copy_file_range=no gl_cv_host_operating_system=GNU/seL4 \
    --enable-single-binary=symlinks --enable-no-install-program=stdbuf \
    --disable-nls --disable-acl --disable-xattr --disable-libcap --without-selinux \
    --without-libgmp --without-openssl --disable-rpath \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
# SUBDIRS=.: sin gnulib-tests ni traducciones (po), que no hacen falta para los programas
make -j"$(nproc)" SUBDIRS=. >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make install SUBDIRS=. DESTDIR="$PWD/inst" >install.log 2>&1 || { tail -20 install.log; exit 1; }
ls inst/usr/local/bin | grep -v '^coreutils$' > programas.txt
echo "coreutils: $(wc -l < programas.txt) programas, binario de $(du -h src/coreutils | cut -f1)"
