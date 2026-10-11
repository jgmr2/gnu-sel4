# port-lib.sh: sourced by the ports' build scripts (ports/*/build-*.sh).
#
#   port_prepare <source> <build-dir> [what else the build depends on...]
#       Sets up <build-dir> and enters it. Returns 0 when it is a fresh copy of <source> (the
#       script then configures it), 1 when the previous build can be reused: only the runtime
#       (crt0.o, libsel4posix.a) changed since, which is linked in at the end. Then the
#       programs linked for GNU/seL4 are deleted and the script's own make links them again,
#       without configuring or compiling anything (seconds instead of minutes).
#       Reused only when nothing that goes into compiling changed: the script, the source
#       directory, the arguments, musl (libc.a, which changes with its headers), sel4-cc and the
#       extra files given (the libraries of other ports whose headers it includes, such as
#       libncursesw.a). PORT_FRESH=1 forces a fresh build.
#   port_done   at the end of a successful build: marks the build directory reusable
#
# SPDX-License-Identifier: BSD-2-Clause

# What compiling depends on, as one hash
_port_key()
{
    {
        echo "$PORT_SCRIPT_KEY"
        echo "$@"
        sha256sum "$SYSROOT/lib/libc.a" "$SYSROOT/bin/sel4-cc" 2>/dev/null
        for f in "$@"; do [ -f "$f" ] && sha256sum "$f"; done
    } | sha256sum | cut -d' ' -f1
}

port_prepare()
{
    _src=$1 _build=$2; shift 2
    PORT_SCRIPT_KEY=$(sha256sum "$0" | cut -d' ' -f1)
    PORT_KEY=$(_port_key "$_src" "$@")
    if [ -z "${PORT_FRESH:-}" ] && [ -f "$_build/.port-key" ] && [ "$(cat "$_build/.port-key")" = "$PORT_KEY" ]; then
        rm -f "$_build/.port-key"           # set again only when this build succeeds
        cd "$_build"
        PORT_DIR=$(pwd)
        # The executables linked for GNU/seL4 (static ELF, type EXEC; the host's own tools are
        # PIE): make links them again with the new runtime
        find . -type f -perm -u+x ! -name '*.o' ! -name '*.a' ! -name '*.so*' | while read -r f; do
            [ "$(od -An -tx1 -N4 "$f" 2>/dev/null | tr -d ' ')" = 7f454c46 ] || continue
            [ "$(od -An -tx1 -j16 -N1 "$f" | tr -d ' ')" = 02 ] && rm -f "$f"
        done
        echo "$(basename "$_build"): relinking with the new runtime (no configure, no compiling)"
        return 1
    fi
    rm -rf "$_build"
    cp -a "$_src" "$_build"
    cd "$_build"
    PORT_DIR=$(pwd)
    return 0
}

port_done() { echo "$PORT_KEY" > "$PORT_DIR/.port-key"; }
