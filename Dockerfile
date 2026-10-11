# Development environment for seL4 Microkit
# Contains: compiler (clang/LLVM), make, gdb. (QEMU runs on the host.)
# The Microkit SDK lives on the host (./sdk) and is mounted at /work/sdk.
#
# SPDX-License-Identifier: BSD-2-Clause
FROM debian:trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang-18 lld-18 llvm-18 libclang-rt-18-dev make device-tree-compiler acpica-tools perl xz-utils cpio \
        gdb-multiarch python3 python3-pip git ca-certificates curl less vim-tiny \
        dosfstools gdisk fdisk e2fsprogs \
    && rm -rf /var/lib/apt/lists/*

# libvmm uses LLVM 18 (clang 19 fails to build its x86_64 code)
ENV PATH=/usr/lib/llvm-18/bin:$PATH

# sdfgen: generates the system description (.system) for sDDF-based systems
RUN pip3 install --break-system-packages sdfgen==0.35.0

ARG UID=1000
ARG GID=1000
RUN groupadd -g ${GID} dev && useradd -m -u ${UID} -g ${GID} -s /bin/bash dev

ENV MICROKIT_SDK=/work/sdk/microkit-sdk-2.3.1 \
    MICROKIT_BOARD=qemu_virt_aarch64 \
    MICROKIT_CONFIG=debug \
    LLVM=True

USER dev
WORKDIR /work
