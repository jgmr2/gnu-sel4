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
RUN apt-get update && apt-get install -y --no-install-recommends qemu-system-x86 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=deps --chown=dev:dev /deps /deps
COPY --from=tool /src/target/release/microkit /deps/third_party/microkit/target/release/microkit
ENV DEPS=/deps
USER dev
