# Dependencies (not in git: they are downloaded)

`./fetch-deps.sh` downloads all of them and checks every SHA-256 and commit against
[`deps.lock`](deps.lock), which is the authoritative list; this table describes it. The Docker
image (`docker compose`, see README.md) carries them already fetched.

| Directory | Source | Version |
|---|---|---|
| `sdk/microkit-sdk-2.3.1` | https://github.com/seL4/microkit/releases | 2.3.1 (prebuilt SDK: kernel, libmicrokit) |
| `third_party/microkit` | https://github.com/seL4/microkit | tag 2.3.1 + `projects/gnu-sel4/microkit/*.patch` (`make microkit-tool`) |
| `libvmm` | https://github.com/au-ts/libvmm | d2e37165285a28c7eae95501594d0807ea482c9c (sDDF in `dep/sddf`: 1fb88897e84f3634e701c6e4a9fc9fe689ac7fbd), with our patches in `projects/gnu-sel4/libvmm/` |
| `third_party/f-stack` | https://github.com/F-Stack/f-stack | v1.25 (761639943bdd), only `lib/`, `freebsd/`, `mk/` |
| `third_party/musl-1.2.6` | https://musl.libc.org/releases/musl-1.2.6.tar.gz | 1.2.6 |
| `third_party/dash-0.5.13.5` | http://gondor.apana.org.au/~herbert/dash/files/ | 0.5.13.5 |
| `third_party/coreutils-9.12` | https://ftp.gnu.org/gnu/coreutils/ | 9.12 |
| `third_party/ncurses-6.6` | https://ftp.gnu.org/gnu/ncurses/ | 6.6 |
| `third_party/zsh-5.9.2` | https://www.zsh.org/pub/ | 5.9.2 |
| `third_party/vim-9.2.1172` | https://github.com/vim/vim | v9.2.1172 |
| `third_party/procps-ng-4.0.7` | https://sourceforge.net/projects/procps-ng/ | 4.0.7 |
| `third_party/mbedtls-3.6.7` | https://github.com/Mbed-TLS/mbedtls/releases | 3.6.7 (LTS) |
| `third_party/curl-8.22.0` | https://curl.se/download/ | 8.22.0 |
| `third_party/cacert/cacert.pem` | https://curl.se/ca/cacert.pem | Mozilla certificates from 2026-09-25 |
| `third_party/socat-1.8.1.3` | http://www.dest-unreach.org/socat/download/ | 1.8.1.3 |
| `third_party/dropbear-2026.94` | https://matt.ucc.asn.au/dropbear/releases/ | 2026.94 |
| `third_party/grep-3.12` | https://ftp.gnu.org/gnu/grep/ | 3.12 |
| `third_party/sed-4.10` | https://ftp.gnu.org/gnu/sed/ | 4.10 |
| `third_party/findutils-4.11.0` | https://ftp.gnu.org/gnu/findutils/ | 4.11.0 |
| `third_party/diffutils-3.12` | https://ftp.gnu.org/gnu/diffutils/ | 3.12 |
| `third_party/tar-1.35` | https://ftp.gnu.org/gnu/tar/ | 1.35 |
| `third_party/gzip-1.15` | https://ftp.gnu.org/gnu/gzip/ | 1.15 |
| `third_party/less-710` | https://www.greenwoodsoftware.com/less/ | 710 |
| `third_party/skalibs-2.15.1.0` | https://skarnet.org/software/skalibs/ | 2.15.1.0 |
| `third_party/execline-2.9.9.2` | https://skarnet.org/software/execline/ | 2.9.9.2 |
| `third_party/s6-2.15.1.0` | https://skarnet.org/software/s6/ | 2.15.1.0 |
| `third_party/s6-rc-0.7.0.0` | https://skarnet.org/software/s6-rc/ | 0.7.0.0 |
| `third_party/sudo-1.9.17p2` | https://www.sudo.ws/dist/ | 1.9.17p2 |
| `third_party/spleen` | https://github.com/fcambus/spleen | 2.2.0 (`spleen-8x16.bdf`, LICENSE) |
| `third_party/qrcodegen` | https://github.com/nayuki/QR-Code-generator | v1.8.0, 720f62bddb7226106071d4728c292cb1df519ceb (`c/`, Readme with the MIT license) |


## The Linux VM's image

`make vm-rootfs` (in `projects/gnu-sel4`, on the host) builds the VM's kernel and initramfs from
the "VM image" section of `deps.lock` only: no network inside the build, and the same bytes on
every build (checked: two builds, one SHA-256). `fetch-deps.sh --no-vm` leaves these out (the
Docker image does).

| Directory | Source | Version |
|---|---|---|
| (container image) | `alpine:3.24` | by digest, the tool that builds the image |
| `third_party/alpine-3.24/vm/` | https://dl-cdn.alpinelinux.org/alpine/v3.24/main | `linux-virt`, `busybox-static`, `ca-certificates-bundle` and their dependencies, each by exact version and SHA-256: what goes into the VM |
| `third_party/alpine-3.24/tools/` | same | `build-base`, `linux-headers`, `kmod`, `iptables`, `e2fsprogs`, `cpio` and the base image's packages: what builds it (and the iptables/e2fsprogs files copied in) |
| `third_party/alpine-3.24/docker-29.9.0.tgz` | https://download.docker.com/linux/static/stable/ | 29.9.0 (static binaries) |

Alpine keeps only the latest build of each package on its mirrors: when a pinned one is
replaced, `fetch-deps.sh` fails on a fresh clone (copies already downloaded keep working).
`make vm-lock` then pins the current ones; the versions it picked are in `deps.lock`.
