# Entorno de desarrollo para seL4 Microkit
# Contiene: compilador (clang/LLVM), make, gdb. (QEMU corre en el host.)
# El SDK de Microkit vive en el host (./sdk) y se monta en /work/sdk.
FROM debian:trixie

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang-18 lld-18 llvm-18 make device-tree-compiler acpica-tools perl xz-utils cpio \
        gdb-multiarch python3 python3-pip git ca-certificates curl less vim-tiny \
        dosfstools gdisk fdisk e2fsprogs \
    && rm -rf /var/lib/apt/lists/*

# libvmm usa LLVM 18 (clang 19 falla al compilar su código x86_64)
ENV PATH=/usr/lib/llvm-18/bin:$PATH

# sdfgen: genera la descripción del sistema (.system) para los ejemplos con sDDF
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
