#!/bin/sh
# Builds a GNU autoconf package (grep, sed, findutils, diffutils, tar, gzip) for GNU/seL4
# with sel4-cc, statically, the same way as coreutils.
#   usage: build-gnu.sh <source> <build-dir> <sysroot> [extra configure arguments...]
# Output: <build-dir>/inst/usr/bin/* (stripped)
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd); shift 3

. "$(dirname "$0")/../port-lib.sh"
if port_prepare "$SRC" "$BUILD" "$@" "$SYSROOT/lib/libncursesw.a"; then
# No Linux kernel headers in the sysroot: copy_file_range is declared missing so that
# gnulib uses its portable version (the Linux one checks <linux/version.h>).
case $(basename "$SRC") in
gzip-*)   # gzip.h's `head` macro would rename a field of aarch64's <bits/signal.h> (struct
          # _aarch64_ctx head): gzip.c includes <signal.h> after it; first, before tailor.h, instead
    sed -i 's|^#include "tailor.h"|#include <signal.h>\n#include "tailor.h"|' gzip.c ;;
esac
./configure --host=${SEL4_HOST:-x86_64-linux-musl} --build=x86_64-pc-linux-gnu --prefix=/usr \
    CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2" \
    ac_cv_func_copy_file_range=no gl_cv_host_operating_system=GNU/seL4 \
    --disable-nls --disable-acl --disable-xattr --without-selinux --disable-rpath "$@" \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
# Without the test suites (gnulib-tests, tests): they are not needed for the programs
for d in gnulib-tests tests; do
    [ -f "$d/Makefile" ] && printf 'all install check clean:\n\t@:\n' > "$d/Makefile"
done
fi
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
make install DESTDIR="$PWD/inst" >install.log 2>&1 || { tail -20 install.log; exit 1; }
mkdir -p bin
for f in inst/usr/bin/*; do
    case "$(head -c 4 "$f" | tail -c 3)" in
    ELF) llvm-strip -o "bin/$(basename "$f")" "$f" ;;
    *)   # egrep, zcat, gunzip... (POSIX scripts; configure wrote the build host's bash into them)
         [ "$(head -c 2 "$f")" = "#!" ] && sed '1s|^#!.*/bash$|#!/bin/sh|' "$f" > "bin/$(basename "$f")" ;;
    esac
done
echo "$(basename "$SRC"): $(ls bin | tr '\n' ' ')"
port_done
