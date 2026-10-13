# GNU/seL4 build environment, in stages:
#   toolchain  compiler (clang/LLVM 18), make, sdfgen, e2fsprogs, GRUB tools. The dependencies stay on the
#              host next to the repository (./dev.sh build uses this stage)
#   full       toolchain + every dependency (fetch-deps.sh, checked against deps.lock) in /deps,
#              the patched Microkit tool already built, and QEMU for `make test`
#              (docker compose uses this stage)
#
# SPDX-License-Identifier: BSD-2-Clause
FROM debian:trixie AS toolchain

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang-18 lld-18 llvm-18 libclang-rt-18-dev make device-tree-compiler acpica-tools perl \
        xz-utils bzip2 cpio gdb-multiarch python3 python3-pip git ca-certificates curl less vim-tiny \
        dosfstools gdisk fdisk e2fsprogs \
    && rm -rf /var/lib/apt/lists/*
# Live ISOs for real hardware (make PLATFORM=latitude7390 iso): GRUB for UEFI and BIOS
RUN apt-get update && apt-get install -y --no-install-recommends \
        grub-common grub-efi-amd64-bin grub-pc-bin xorriso mtools \
    && rm -rf /var/lib/apt/lists/*

# ccache: tools/sel4-cc compiles through it when CCACHE_DIR is set (dev.sh, docker compose)
RUN apt-get update && apt-get install -y --no-install-recommends ccache \
    && rm -rf /var/lib/apt/lists/*

# libvmm uses LLVM 18 (clang 19 fails to build its x86_64 code)
ENV PATH=/usr/lib/llvm-18/bin:$PATH

# sdfgen: generates the system description (.system) for sDDF-based systems
RUN pip3 install --break-system-packages sdfgen==0.35.0

ARG UID=1000
ARG GID=1000
RUN groupadd -g ${GID} dev && useradd -m -u ${UID} -g ${GID} -s /bin/bash dev

ENV MICROKIT_CONFIG=debug \
    LLVM=True

# The ARM64 build (make PLATFORM=qemu-arm): libgcc for aarch64, the routines the compiler calls on
# its own (Debian ships compiler-rt for the host only; tools/sel4-cc uses this one there)
RUN apt-get update && apt-get install -y --no-install-recommends libgcc-14-dev-arm64-cross \
    && rm -rf /var/lib/apt/lists/*
# The browser port (TODO item 24): compiler-rt for wasm32 (long double is 128-bit there: musl's
# printf needs its soft float), which tools/sel4-cc links into WebAssembly programs
RUN apt-get update && apt-get install -y --no-install-recommends libclang-rt-18-dev-wasm32 \
    && rm -rf /var/lib/apt/lists/*

USER dev
WORKDIR /work

# ---------------------------------------------------------------- dependencies
FROM toolchain AS deps
USER root
COPY deps.lock fetch-deps.sh /deps/
COPY projects/gnu-sel4/microkit/*.patch /deps/projects/gnu-sel4/microkit/
COPY projects/gnu-sel4/libvmm/*.patch /deps/projects/gnu-sel4/libvmm/
RUN /deps/fetch-deps.sh --no-vm && rm -rf /deps/.cache /deps/projects

# ---------------------------------------------------------------- the patched Microkit tool
FROM rust:1.94 AS tool
COPY --from=deps /deps/third_party/microkit /src
RUN cd /src && cargo build --release -p microkit-tool

# ---------------------------------------------------------------- everything together
FROM toolchain AS full
USER root
# QEMU for both architectures; websockify and noVNC for the browser page (tools/web)
RUN apt-get update && apt-get install -y --no-install-recommends qemu-system-x86 qemu-system-arm websockify novnc \
    && rm -rf /var/lib/apt/lists/*
COPY --from=deps --chown=dev:dev /deps /deps
COPY --from=tool /src/target/release/microkit /deps/third_party/microkit/target/release/microkit
ENV DEPS=/deps
USER dev

# The browser page only (./web.sh, docker compose up web): QEMU for both architectures, make,
# websockify and noVNC; it boots what ../../dev.sh make built, so it needs none of the toolchain
FROM debian:trixie AS web
RUN apt-get update && apt-get install -y --no-install-recommends make python3 qemu-system-x86 qemu-system-arm \
        websockify novnc \
    && rm -rf /var/lib/apt/lists/*
