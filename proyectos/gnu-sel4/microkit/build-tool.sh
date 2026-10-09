#!/bin/sh
# Herramienta de Microkit 2.3.1 con nuestro parche (<cap_untyped>, <cap_cnode> en <cspace>).
# Se ejecuta en el host: clona el código si falta, aplica el parche y compila con Rust en Docker.
#   Salida: third_party/microkit/target/release/microkit
set -eu
cd "$(dirname "$0")"
PATCHES=$(pwd)
SRC=../../../third_party/microkit
if [ ! -d "$SRC" ]; then
    git clone -q --depth 1 --branch 2.3.1 https://github.com/seL4/microkit "$SRC"
    (cd "$SRC" && git checkout -q -b gnu-sel4 && git am -q "$PATCHES"/*.patch)
fi
cd "$SRC"
docker run --rm -u "$(id -u):$(id -g)" -e CARGO_HOME=/w/.cargo -v "$(pwd)":/w -w /w rust:1.94 \
    cargo build --release -p microkit-tool
