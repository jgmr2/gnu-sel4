#!/bin/sh
# dropbear's ssh client for GNU/seL4: dbclient (ssh), dropbearkey and scp, in a single
# multi-call binary. The server (dropbear) needs pseudo-terminals: not yet.
#   usage: build-dropbear.sh <source> <build-dir> <sysroot>     Output: <dir>/dropbearmulti
set -eu
SRC=$(cd "$1" && pwd); BUILD=$2; SYSROOT=$(cd "$3" && pwd)
rm -rf "$BUILD"; cp -a "$SRC" "$BUILD"; cd "$BUILD"
# scp calls ssh by its path: here /bin/dbclient (symlink to the multi-call binary)
./configure --host=x86_64-linux-musl CC="$SYSROOT/bin/sel4-cc" \
    CFLAGS="-O2 -DDROPBEAR_PATH_SSH_PROGRAM='\"/bin/dbclient\"'" \
    --disable-zlib --disable-syslog --disable-lastlog --disable-utmp --disable-utmpx \
    --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline --disable-pam \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"$(nproc)" PROGRAMS="dbclient dropbearkey scp" MULTI=1 >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }
echo "dropbear: $(du -h dropbearmulti | cut -f1)"
