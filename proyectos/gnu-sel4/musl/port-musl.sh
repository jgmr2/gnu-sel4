#!/bin/sh
# Prepara una copia de musl portada a seL4 y la compila.
#   uso: port-musl.sh <fuente-musl> <dir-trabajo> <prefijo-instalación>
#
# El port consiste en:
#   1. arch/x86_64/syscall_arch.h: en vez de la instrucción `syscall` (que
#      entraría al kernel de seL4, no a Linux) cada __syscallN llama a
#      __sel4_syscall(), que implementa el runtime (proyectos/posix/runtime).
#   2. Borrar el ensamblador x86_64 que usa `syscall` directamente; musl tiene
#      versiones genéricas en C de todos ellos que pasan por __syscall().
set -eu
SRC=$1; WORK=$2; PREFIX=$3

rm -rf "$WORK"; cp -a "$SRC" "$WORK"; cd "$WORK"

cat > arch/x86_64/syscall_arch.h <<'EOH'
/* seL4: las llamadas al sistema se traducen a IPC en el runtime POSIX */
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

/* Sin vDSO: no hay kernel Linux que lo provea */

#define IPC_64 0
EOH

rm -f src/process/x86_64/vfork.s src/signal/x86_64/restore.s \
      src/thread/x86_64/clone.s src/thread/x86_64/__set_thread_area.s \
      src/thread/x86_64/syscall_cp.s src/thread/x86_64/__unmapself.s
if grep -rln '^\s*syscall\b' src/*/x86_64/; then
    echo "port-musl: aún queda ensamblador con 'syscall'" >&2; exit 1
fi

CC="clang --target=x86_64-linux-musl" AR=llvm-ar RANLIB=llvm-ranlib \
CFLAGS="-O2 -fno-pic -fno-stack-protector" \
    ./configure --target=x86_64-linux-musl --prefix="$PREFIX" --disable-shared --disable-wrapper >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "musl instalada en $PREFIX"
