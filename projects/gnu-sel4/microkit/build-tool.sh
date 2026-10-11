#!/bin/sh
# Microkit 2.3.1 tool with our patches (<cap_untyped>, <cap_cnode>... in <cspace>).
# Runs on the host: builds the patched source (from fetch-deps.sh) with Rust in Docker.
#   Output: third_party/microkit/target/release/microkit
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")"
SRC=${DEPS:-../../..}/third_party/microkit
# fetch-deps.sh clones it at tag 2.3.1 and applies the patches in this directory
[ -d "$SRC" ] || { echo "Missing $SRC: run ./fetch-deps.sh at the top of the repository"; exit 1; }
cd "$SRC"
docker run --rm -u "$(id -u):$(id -g)" -e CARGO_HOME=/w/.cargo -v "$(pwd)":/w -w /w rust:1.94 \
    cargo build --release -p microkit-tool
