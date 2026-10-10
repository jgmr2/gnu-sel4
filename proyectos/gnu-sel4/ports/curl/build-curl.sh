#!/bin/sh
# curl con HTTPS para GNU/seL4: mbedTLS 3.6 (se instala en el sysroot, para otros programas
# también) y curl encima, estático, sin hilos (resolución de nombres síncrona).
#   uso: build-curl.sh <fuente-mbedtls> <fuente-curl> <bundle-de-CA> <dir-de-build> <sysroot>
# Salida: <dir>/curl/src/curl y <dir>/ssl/ca-certificates.crt
set -eu
MBED=$(cd "$1" && pwd); CURL=$(cd "$2" && pwd); CA=$(realpath "$3"); BUILD=$4; SYSROOT=$(cd "$5" && pwd)
CC="$SYSROOT/bin/sel4-cc"

rm -rf "$BUILD"; mkdir -p "$BUILD"; BUILD=$(cd "$BUILD" && pwd)

# mbedTLS: solo la biblioteca (libmbedtls, libmbedx509, libmbedcrypto)
cp -a "$MBED" "$BUILD/mbedtls"
make -C "$BUILD/mbedtls/library" -j"$(nproc)" CC="$CC" AR=llvm-ar CFLAGS="-O2" static \
    >"$BUILD/mbedtls.log" 2>&1 || { tail -20 "$BUILD/mbedtls.log"; exit 1; }
cp -r "$BUILD/mbedtls/include/mbedtls" "$BUILD/mbedtls/include/psa" "$SYSROOT/include/"
cp "$BUILD/mbedtls/library/"libmbed*.a "$SYSROOT/lib/"

# curl: HTTP(S) y FTP; nada que necesite hilos ni bibliotecas que no hay
cp -a "$CURL" "$BUILD/curl"
cd "$BUILD/curl"
./configure --host=x86_64-linux-musl CC="$CC" CFLAGS="-O2" \
    --with-mbedtls="$SYSROOT" --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --without-ca-path \
    --disable-shared --enable-static --disable-threaded-resolver --disable-docs --disable-manual \
    --without-libpsl --without-zlib --without-brotli --without-zstd --without-nghttp2 --without-nghttp3 \
    --without-ngtcp2 --without-libidn2 --without-libssh2 --without-librtmp \
    --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp \
    --disable-pop3 --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
make -j"$(nproc)" >make.log 2>&1 || { grep -E "error|Error" make.log | head -30; exit 1; }

mkdir -p "$BUILD/ssl"
cp "$CA" "$BUILD/ssl/ca-certificates.crt"
echo "curl: $(du -h src/curl | cut -f1), $(grep -c 'BEGIN CERT' "$CA") certificados"
