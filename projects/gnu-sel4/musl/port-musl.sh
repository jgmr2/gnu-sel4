#!/bin/sh
# Prepares a copy of musl ported to seL4 and builds it.
#   usage: port-musl.sh <musl-source> <work-dir> <install-prefix> [x86_64|aarch64|wasm32]
#
# wasm32 (the browser port, TODO item 24) is an architecture musl does not have: wasm32/ here is
# its arch/ and its source (see wasm32/README).
#
# The port consists of:
#   1. arch/<arch>/syscall_arch.h: instead of the `syscall` (x86_64) or `svc` (aarch64)
#      instruction (which would enter the seL4 kernel, not Linux) each __syscallN calls
#      __sel4_syscall(), implemented by the runtime (projects/gnu-sel4/runtime).
#   2. Deleting the assembly that enters the kernel directly; musl has generic C versions of
#      all of it that go through __syscall().
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$1; WORK=$2; PREFIX=$3; ARCH=${4:-x86_64}
HERE=$(cd "$(dirname "$0")" && pwd)

rm -rf "$WORK"; cp -a "$SRC" "$WORK"; cd "$WORK"
if [ "$ARCH" = wasm32 ]; then
    cp -a "$HERE/wasm32/arch" arch/wasm32
    for d in "$HERE"/wasm32/src/*/; do
        mkdir -p "src/$(basename "$d")/wasm32" && cp "$d"* "src/$(basename "$d")/wasm32/"
    done
    sed -i 's/^arm\*) ARCH=arm ;;$/&\nwasm32*) ARCH=wasm32 ;;/' configure
fi

cat > arch/$ARCH/syscall_arch.h <<'EOH'
/* seL4: system calls are translated to IPC in the POSIX runtime */
#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

long __sel4_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

static __inline long __syscall0(long n)
{ return __sel4_syscall(n, 0, 0, 0, 0, 0, 0); }
static __inline long __syscall1(long n, long a1)
{ return __sel4_syscall(n, a1, 0, 0, 0, 0, 0); }
static __inline long __syscall2(long n, long a1, long a2)
{ return __sel4_syscall(n, a1, a2, 0, 0, 0, 0); }
static __inline long __syscall3(long n, long a1, long a2, long a3)
{ return __sel4_syscall(n, a1, a2, a3, 0, 0, 0); }
static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{ return __sel4_syscall(n, a1, a2, a3, a4, 0, 0); }
static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{ return __sel4_syscall(n, a1, a2, a3, a4, a5, 0); }
static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{ return __sel4_syscall(n, a1, a2, a3, a4, a5, a6); }

/* No vDSO: there is no Linux kernel to provide it */

#define IPC_64 0
EOH

# (aarch64's __set_thread_area is C writing tpidr_el0 itself: it stays)
rm -f src/process/$ARCH/vfork.s src/signal/$ARCH/restore.s \
      src/thread/$ARCH/clone.s src/thread/x86_64/__set_thread_area.s \
      src/thread/$ARCH/syscall_cp.s src/thread/$ARCH/__unmapself.s
if grep -rlnE '^\s*(syscall|svc)\b' src/*/$ARCH/; then
    echo "port-musl: there is still assembly entering the kernel" >&2; exit 1
fi
case $ARCH in
aarch64) EXTRA=-mstrict-align; TARGET=$ARCH-linux-musl ;;
wasm32)  EXTRA="-matomics -mbulk-memory -mexception-handling -mllvm -wasm-enable-sjlj"; TARGET=wasm32 ;;   # a shared memory; setjmp as exceptions
*)       EXTRA=; TARGET=$ARCH-linux-musl ;;
esac

CC="clang --target=$TARGET" AR=llvm-ar RANLIB=llvm-ranlib \
CFLAGS="-O2 -fno-pic -fno-stack-protector $EXTRA" \
    ./configure --target=$ARCH-linux-musl --prefix="$PREFIX" --disable-shared --disable-wrapper >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "musl installed in $PREFIX"
