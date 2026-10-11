#!/bin/sh
# Prepares a copy of musl ported to seL4 and builds it.
#   usage: port-musl.sh <musl-source> <work-dir> <install-prefix>
#
# The port consists of:
#   1. arch/x86_64/syscall_arch.h: instead of the `syscall` instruction (which
#      would enter the seL4 kernel, not Linux) each __syscallN calls
#      __sel4_syscall(), implemented by the runtime (projects/gnu-sel4/runtime).
#   2. Deleting the x86_64 assembly that uses `syscall` directly; musl has
#      generic C versions of all of it that go through __syscall().
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
SRC=$1; WORK=$2; PREFIX=$3

rm -rf "$WORK"; cp -a "$SRC" "$WORK"; cd "$WORK"

cat > arch/x86_64/syscall_arch.h <<'EOH'
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

rm -f src/process/x86_64/vfork.s src/signal/x86_64/restore.s \
      src/thread/x86_64/clone.s src/thread/x86_64/__set_thread_area.s \
      src/thread/x86_64/syscall_cp.s src/thread/x86_64/__unmapself.s
if grep -rln '^\s*syscall\b' src/*/x86_64/; then
    echo "port-musl: there is still assembly using 'syscall'" >&2; exit 1
fi

CC="clang --target=x86_64-linux-musl" AR=llvm-ar RANLIB=llvm-ranlib \
CFLAGS="-O2 -fno-pic -fno-stack-protector" \
    ./configure --target=x86_64-linux-musl --prefix="$PREFIX" --disable-shared --disable-wrapper >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "musl installed in $PREFIX"
