# Third-party software

GNU/seL4 is mostly an assembly of existing projects. This file credits every one of them.
Each keeps its own license and copyright; the code written for GNU/seL4 itself is under the
BSD-2-Clause license in [`LICENSE`](LICENSE). Exact versions and download locations are in
[`DEPENDENCIES.md`](DEPENDENCIES.md).

On the installed system, the license texts of the programs on the disk are in
`/usr/share/licenses/<project>/`.

## Kernel and system framework

| Project | Authors | License | Used for |
|---|---|---|---|
| [seL4](https://sel4.systems) | Data61/CSIRO, the seL4 Foundation, Proofcraft and contributors | GPL-2.0-only (kernel); BSD-2-Clause (libsel4 headers) | the microkernel |
| [Microkit](https://github.com/seL4/microkit) | Breakaway Consulting, UNSW, the seL4 Foundation and contributors | BSD-2-Clause | system framework and tool (with our patches in `projects/gnu-sel4/microkit/`) |
| [sDDF](https://github.com/au-ts/sddf) | UNSW Trustworthy Systems and contributors | BSD-2-Clause | drivers and virtualisers (serial, disk, network, timer) |
| [xterm.js](https://xtermjs.org) | The xterm.js authors | MIT | the browser page's terminal (`./web.sh`, `tools/web`) |
| [noVNC](https://novnc.com) and [websockify](https://github.com/novnc/websockify) | The noVNC authors | MPL-2.0 (noVNC), LGPL-3.0 (websockify) | the browser page's screen and WebSocket bridges (Debian packages in the `web` image, not redistributed) |
| [libvmm](https://github.com/au-ts/libvmm) | UNSW Trustworthy Systems and contributors | BSD-2-Clause | the VMM for the Linux VM; `vm/client_vmm.c`, `vm/guest_arch_init.*` and `vm/virtio_pci_dsdt.dsl` are based on its examples and keep UNSW's copyright |
| [sdfgen](https://github.com/au-ts/microkit_sdf_gen) | UNSW Trustworthy Systems | BSD-2-Clause | generating the system description (build time) |

## Native userland

| Project | Authors | License | Used for |
|---|---|---|---|
| [musl](https://musl.libc.org) | Rich Felker and contributors | MIT | the C library (ported to seL4) |
| [F-Stack](https://github.com/F-Stack/f-stack) | THL A29 Limited (Tencent) | BSD-2-Clause | its FreeBSD stack extraction; `net/host.c` derives from its `lib/ff_host_interface.c` |
| [FreeBSD](https://www.freebsd.org) | The FreeBSD Project, the Regents of the University of California and contributors | BSD-2-Clause / BSD-3-Clause | the TCP/IP stack in netstack; `net/compat/sys/queue.h` |
| [dash](http://gondor.apana.org.au/~herbert/dash/) | Herbert Xu, Kenneth Almquist, the Regents of the University of California, Christos Zoulas | BSD-3-Clause | `/bin/sh` (with our own BSD `mksignames.c` instead of bash's GPL one) |
| [zsh](https://www.zsh.org) | Paul Falstad and the zsh development group | MIT-style zsh licence (some shell functions are GPL) | the login shell |
| [Vim](https://www.vim.org) | Bram Moolenaar and the Vim community | Vim license (GPL-compatible charityware) | the editor |
| [ncurses](https://invisible-island.net/ncurses/) | Thomas E. Dickey, the Free Software Foundation | MIT (X11) | terminal library and terminfo |
| [GNU coreutils](https://www.gnu.org/software/coreutils/) | Free Software Foundation | GPL-3.0-or-later | ls, cp, mv, cat… |
| [GNU grep](https://www.gnu.org/software/grep/) | Free Software Foundation | GPL-3.0-or-later | grep |
| [GNU sed](https://www.gnu.org/software/sed/) | Free Software Foundation | GPL-3.0-or-later | sed |
| [GNU findutils](https://www.gnu.org/software/findutils/) | Free Software Foundation | GPL-3.0-or-later | find, xargs |
| [GNU diffutils](https://www.gnu.org/software/diffutils/) | Free Software Foundation | GPL-3.0-or-later | diff, cmp, diff3, sdiff |
| [GNU tar](https://www.gnu.org/software/tar/) | Free Software Foundation | GPL-3.0-or-later | tar |
| [GNU gzip](https://www.gnu.org/software/gzip/) | Free Software Foundation | GPL-3.0-or-later | gzip, zcat… |
| [less](https://www.greenwoodsoftware.com/less/) | Mark Nudelman | GPL-3.0-or-later or the Less License | the pager |
| [e2fsprogs](https://e2fsprogs.sourceforge.net) | Theodore Ts'o and contributors | GPL-2.0 (libext2fs, libe2p: LGPL-2.0; libuuid: BSD-3-Clause; libet, libss: MIT) | e2fsck, mke2fs, tune2fs, dumpe2fs, resize2fs |
| [GNU screen](https://www.gnu.org/software/screen/) | Free Software Foundation and contributors | GPL-3.0-or-later | the terminal multiplexer |
| [procps-ng](https://gitlab.com/procps-ng/procps) | the procps-ng developers | GPL-2.0-or-later (library: LGPL-2.1-or-later) | ps, top, free… |
| [curl](https://curl.se) | Daniel Stenberg and contributors | curl license (MIT-style) | curl |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) | the Mbed TLS contributors (Arm, TrustedFirmware) | Apache-2.0 OR GPL-2.0-or-later (used under Apache-2.0) | TLS for curl |
| [Mozilla CA certificates](https://curl.se/docs/caextract.html) | Mozilla, extracted by the curl project | MPL-2.0 | `/etc/ssl/certs/ca-certificates.crt` |
| [socat](http://www.dest-unreach.org/socat/) | Gerhard Rieger and contributors | GPL-2.0 | socat and `nc` |
| [Dropbear](https://matt.ucc.asn.au/dropbear/dropbear.html) | Matt Johnston and contributors (includes libtomcrypt/libtommath, public domain) | MIT | ssh, scp, dropbearkey |
| [skalibs, execline, s6, s6-rc](https://skarnet.org/software/) | Laurent Bercot (skarnet.org) | ISC | service supervision (`s6-svscan`, `s6-svc`…), the execline language |
| [sudo](https://www.sudo.ws) | Todd C. Miller and contributors | ISC-style (sudo license) | root work from a normal account |
| [Spleen](https://github.com/fcambus/spleen) | Frederic Cambus | BSD-2-Clause | the console font |
| [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) | Project Nayuki | MIT | `qrlog`: test logs out of the live system as QR codes |

## Linux VM

| Project | Authors | License | Used for |
|---|---|---|---|
| [Linux](https://kernel.org) (Alpine's linux-virt) | Linus Torvalds and the kernel contributors | GPL-2.0-only | the VM kernel |
| [BusyBox](https://busybox.net) | Erik Andersen, Rob Landley, Denys Vlasenko and contributors | GPL-2.0-only | the VM's userland |
| [Docker](https://www.docker.com) (Moby, Docker CLI, containerd, runc) | Docker, Inc. and the Moby, containerd and runc contributors | Apache-2.0 | containers |
| [iptables](https://netfilter.org) | the Netfilter project | GPL-2.0-or-later | Docker's networking |
| [e2fsprogs](https://e2fsprogs.sourceforge.net) | Theodore Ts'o and contributors | GPL-2.0 (libraries: LGPL/BSD/MIT) | formatting and checking the Docker disk |
| [Alpine Linux](https://alpinelinux.org) | the Alpine Linux developers | various (per package) | packages the VM image is built from |

## Build tools (not shipped, except as noted)

| Project | License | Note |
|---|---|---|
| [LLVM/Clang, lld](https://llvm.org) | Apache-2.0 WITH LLVM-exception | compiler; compiler-rt is linked into the programs (the LLVM exception allows this without attribution requirements) |
| [Debian](https://www.debian.org), [Docker](https://www.docker.com), [QEMU](https://www.qemu.org), Rust | various | build environment and emulator |

## Redistribution notes

- Distributing a GNU/seL4 disk image means distributing the GPL programs above: whoever does so
  must also offer their source code. This repository builds everything from source, with the
  exact versions listed in `DEPENDENCIES.md`.
- The programs are only aggregated with GNU/seL4's own code; the runtime that every program links
  (`libsel4posix.a`) is BSD-2-Clause, compatible with all of the licenses above.
