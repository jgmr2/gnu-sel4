#!/bin/sh
# Microkit 2.3.1 tool with our patches (<cap_untyped>, <cap_cnode>... in <cspace>).
# Runs on the host: clones the source if missing, applies the patches and builds with Rust in Docker.
#   Output: third_party/microkit/target/release/microkit
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
