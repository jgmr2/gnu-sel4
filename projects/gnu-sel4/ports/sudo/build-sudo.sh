#!/bin/sh
# Builds sudo for GNU/seL4 with sel4-cc: one static binary with the sudoers policy inside
# (no plugins to load: there is no dynamic linking), passwords checked against /etc/shadow
# (no PAM), no noexec/intercept (they are LD_PRELOAD libraries), no mail, no log server.
#   usage: build-sudo.sh <sudo-source> <build-dir> <sysroot>
# Output: <build-dir>/inst/usr/bin/sudo, visudo...
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)
. "$(dirname "$0")/../port-lib.sh"
if port_prepare "$SRC" "$BUILD"; then
./configure --host=${SEL4_HOST:-x86_64-linux-musl} --build=x86_64-pc-linux-gnu --prefix=/usr --sysconfdir=/etc \
    --localstatedir=/var --with-rundir=/run/sudo --with-vardir=/var/db/sudo \
    CC="$SYSROOT/bin/sel4-cc" CFLAGS="-O2" \
    --disable-shared --enable-static-sudoers --disable-shared-libutil \
    --without-pam --disable-pam-session --with-passwd --without-sendmail --disable-nls \
    --disable-log-server --disable-log-client --disable-noexec --disable-intercept \
    --without-sssd --without-ldap --enable-tmpfiles.d=no --with-editor=/bin/vi --with-env-editor \
    --with-secure-path=/bin:/sbin:/usr/bin --with-logpath=/var/log/sudo.log \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
fi
make -j"$(nproc)" >make.log 2>&1 || { grep -E ": error:|\*\*\*" make.log | head -20; exit 1; }
make install DESTDIR="$PWD/inst" INSTALL_OWNER= >install.log 2>&1 || { tail -20 install.log; exit 1; }
mkdir -p bin
for p in sudo visudo; do llvm-strip -o "bin/$p" "inst/usr/bin/$p" 2>/dev/null || llvm-strip -o "bin/$p" "inst/usr/sbin/$p"; done
echo "sudo: $(du -h bin/sudo | cut -f1)"
port_done
