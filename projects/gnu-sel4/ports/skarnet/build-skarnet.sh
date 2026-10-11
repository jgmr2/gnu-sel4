#!/bin/sh
# Builds a skarnet.org package (skalibs, execline, s6, s6-rc) for GNU/seL4 with sel4-cc, static.
# skalibs goes into the sysroot (library, headers, sysdeps); the others link against it and
# install their programs into <build-dir>/inst.
#   usage: build-skarnet.sh <source> <build-dir> <sysroot>
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)
rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
NAME=$(basename "$SRC"); NAME=${NAME%-*}
CC="$SYSROOT/bin/sel4-cc"
TOOLS="AR=llvm-ar RANLIB=llvm-ranlib STRIP=llvm-strip"     # skalibs would use x86_64-linux-musl-ar...

if [ "$NAME" = skalibs ]; then
    # Cross compilation: the sysdeps configure cannot test by running programs. Also "no" for
    # what musl declares but posix_server does not implement yet, so skalibs uses its portable
    # fallbacks: signalfd (self-pipe), posix_spawn (fork + exec), waitid, splice, sendfile,
    # namespaces, chroot, settimeofday.
    ./configure --host=x86_64-linux-musl --prefix=/ --libdir=/lib --includedir=/include \
        --sysdepdir=/lib/skalibs/sysdeps --enable-static --disable-shared \
        --with-sysdep-devurandom=yes --with-sysdep-posixspawnearlyreturn=no \
        --with-sysdep-procselfexe=/proc/self/exe --with-sysdep-selectinfinite=yes \
        --with-sysdep-signalfd=no --with-sysdep-posixspawn=no --with-sysdep-waitid=no \
        --with-sysdep-splice=no --with-sysdep-sendfile=no --with-sysdep-namespaces=no \
        --with-sysdep-nsgetparent=no --with-sysdep-chroot=no --with-sysdep-settimeofday=no \
        CC="$CC" CFLAGS="-O2" AR=llvm-ar RANLIB=llvm-ranlib >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    make -j"$(nproc)" $TOOLS >make.log 2>&1 || { grep -E ": error:|\*\*\*" make.log | head -20; exit 1; }
    make install $TOOLS DESTDIR="$SYSROOT" >install.log 2>&1 || { tail -20 install.log; exit 1; }
    echo "skalibs: $(ls "$SYSROOT/lib/libskarnet.a")"
else
    MULTI=; [ "$NAME" = execline ] && MULTI=--enable-multicall      # one binary + symlinks
    ./configure --host=x86_64-linux-musl --prefix=/ --bindir=/bin --libexecdir=/usr/libexec --enable-static-libc --disable-shared $MULTI \
        --with-sysdeps="$SYSROOT/lib/skalibs/sysdeps" --with-include="$SYSROOT/include" \
        --with-lib="$SYSROOT/lib" --with-lib="$SYSROOT/lib/skalibs" \
        CC="$CC" CFLAGS="-O2" AR=llvm-ar RANLIB=llvm-ranlib >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    make -j"$(nproc)" $TOOLS >make.log 2>&1 || { grep -E ": error:|\*\*\*" make.log | head -20; exit 1; }
    make install $TOOLS DESTDIR="$PWD/inst" >install.log 2>&1 || { tail -20 install.log; exit 1; }
    # its library and headers into the sysroot too (s6 needs execline's, s6-rc needs s6's)
    [ -d inst/include ] && cp -r inst/include/. "$SYSROOT/include/"
    find inst -name '*.a' -exec cp {} "$SYSROOT/lib/" \;
    # programs -> bin/; helpers (s6-ftrigrd...) -> libexec/; symlinks (execline's multicall
    # names) -> links.txt as "name target"
    mkdir -p bin libexec; : > links.txt
    for f in inst/usr/libexec/*; do [ ! -f "$f" ] || llvm-strip -o "libexec/$(basename "$f")" "$f"; done
    for f in inst/bin/*; do
        if [ -L "$f" ]; then echo "$(basename "$f") $(basename "$(readlink "$f")")" >> links.txt
        elif [ -f "$f" ]; then llvm-strip -o "bin/$(basename "$f")" "$f"; fi
    done
    echo "$NAME: $(ls bin | wc -l) binaries + $(wc -l < links.txt) links, $(du -sh bin | cut -f1)"
fi
