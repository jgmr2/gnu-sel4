#!/bin/sh
# Une dos discos de una partición (MBR, inicio en 1 MiB) en uno de dos particiones:
#   1: ext2 de GNU/seL4 (64 MiB)    <- partición del disco viejo de posix
#   2: Docker de la VM               <- partición del disco viejo de docker-vm
# uso: migrar-discos.sh disco-ext2.img disco-docker.img nuevo.img
set -eu
EXT2=$1 DOCKER=$2 OUT=$3
EXT2_MB=64
docker_sectors=$(( $(stat -c %s "$DOCKER") / 512 - 2048 ))
truncate -s $(( 1 + EXT2_MB ))M "$OUT.tmp"
truncate -s +$(( docker_sectors * 512 )) "$OUT.tmp"
printf 'label: dos\nstart=2048, size=%sMiB, type=83\ntype=83\n' $EXT2_MB | /sbin/sfdisk -q "$OUT.tmp"
# partición 1: el ext2 viejo (63 MiB) cabe en la nueva de 64 MiB
dd if="$EXT2" of="$OUT.tmp" bs=1M skip=1 seek=1 conv=notrunc,sparse status=none
# partición 2: el disco de Docker, copiando solo lo usado (sparse)
start=$(/sbin/sfdisk -d "$OUT.tmp" | awk -F'[=,]' '/img.tmp2/ {gsub(/ /,"",$2); print $2}')
dd if="$DOCKER" of="$OUT.tmp" bs=512 skip=2048 seek="$start" conv=notrunc,sparse status=none
mv "$OUT.tmp" "$OUT"
/sbin/sfdisk -l "$OUT" | tail -3
