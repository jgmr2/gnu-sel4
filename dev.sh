#!/usr/bin/env bash
# Uso:
#   ./dev.sh build        construye la imagen Docker (solo la primera vez)
#   ./dev.sh              abre una terminal dentro del entorno de desarrollo
#   ./dev.sh <comando>    ejecuta un comando dentro del entorno, en la carpeta actual
#                         (p. ej. desde proyectos/hola: ../../dev.sh make)
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
IMAGE=sel4-dev

if [ "$1" = "build" ]; then
    exec docker build --build-arg UID="$(id -u)" --build-arg GID="$(id -g)" -t "$IMAGE" "$ROOT"
fi

# Carpeta actual relativa a la raíz del proyecto (si está dentro de ella)
case "$PWD/" in
    "$ROOT"/*) REL="${PWD#"$ROOT"}" ;;
    *)         REL="" ;;
esac

TTY=(-i); [ -t 0 ] && TTY=(-it)
[ $# -eq 0 ] && set -- bash
exec docker run --rm "${TTY[@]}" -v "$ROOT":/work -w "/work$REL" "$IMAGE" "$@"
