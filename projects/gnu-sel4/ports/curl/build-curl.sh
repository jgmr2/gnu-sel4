#!/bin/sh
# curl with HTTPS for GNU/seL4: mbedTLS 3.6 (installed into the sysroot, for other programs
# too) and curl on top, static, without threads (synchronous name resolution).
#   usage: build-curl.sh <mbedtls-source> <curl-source> <CA-bundle> <build-dir> <sysroot>
# Output: <dir>/curl/src/curl and <dir>/ssl/ca-certificates.crt
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
MBED=$(cd "$1" && pwd); CURL=$(cd "$2" && pwd); CA=$(realpath "$3"); BUILD=$4; SYSROOT=$(cd "$5" && pwd)
CC="$SYSROOT/bin/sel4-cc"

mkdir -p "$BUILD"; BUILD=$(cd "$BUILD" && pwd)
. "$(dirname "$0")/../port-lib.sh"
if port_prepare "$CURL" "$BUILD/curl" "$MBED"; then
# mbedTLS: just the library (libmbedtls, libmbedx509, libmbedcrypto)
rm -rf "$BUILD/mbedtls"; cp -a "$MBED" "$BUILD/mbedtls"
make -C "$BUILD/mbedtls/library" -j"$(nproc)" CC="$CC" AR=llvm-ar CFLAGS="-O2" static \
    >"$BUILD/mbedtls.log" 2>&1 || { tail -20 "$BUILD/mbedtls.log"; exit 1; }
cp -r "$BUILD/mbedtls/include/mbedtls" "$BUILD/mbedtls/include/psa" "$SYSROOT/include/"
cp "$BUILD/mbedtls/library/"libmbed*.a "$SYSROOT/lib/"

# curl: HTTP(S) and FTP; nothing that needs threads or libraries we do not have
./configure --host=x86_64-linux-musl CC="$CC" CFLAGS="-O2" \
    --with-mbedtls="$SYSROOT" --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --without-ca-path \
    --disable-shared --enable-static --disable-threaded-resolver --disable-docs --disable-manual \
    --without-libpsl --without-zlib --without-brotli --without-zstd --without-nghttp2 --without-nghttp3 \
    --without-ngtcp2 --without-libidn2 --without-libssh2 --without-librtmp \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
    --disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
fi
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }

mkdir -p "$BUILD/ssl"
cp "$CA" "$BUILD/ssl/ca-certificates.crt"
echo "curl: $(du -h src/curl | cut -f1), $(grep -c 'BEGIN CERT' "$CA") certificates"
port_done
