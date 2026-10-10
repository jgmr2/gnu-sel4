# GNU/seL4

Un sistema tipo Unix sobre el microkernel seL4: dash y programas POSIX nativos, con una VM
Linux al lado para Docker, integrada de forma transparente (como WSL2). Plan y estado: `TODO.md`.

```
sdk/                 Microkit SDK 2.3.1 (kernel seL4 + herramienta + libmicrokit)
third_party/microkit código de Microkit 2.3.1 con nuestro parche a la herramienta (make microkit-tool)
libvmm/              libvmm (VMM de seL4) con sDDF en libvmm/dep/sddf (drivers y virtualizadores)
third_party/         fuentes de terceros: musl, dash, coreutils, ncurses, zsh, vim, F-Stack (pila de FreeBSD)
proyectos/gnu-sel4/  el sistema
Dockerfile, dev.sh   entorno de compilación (clang/LLVM 18, make, dtc, iasl, sdfgen, e2fsprogs)
```

## Uso

```sh
./dev.sh build                  # imagen Docker de compilación (una vez)
cd proyectos/gnu-sel4
make microkit-tool              # (host, una vez) herramienta de Microkit con nuestro parche
make vm-rootfs                  # (host, una vez) kernel + initramfs de la VM
../../dev.sh make               # compila todo y lo instala en disk.img
make run                        # QEMU + KVM con consola gráfica (ventana); salir: Ctrl-a x en la terminal
make run CONSOLA=serie          # todo por el puerto serie (compilar también con CONSOLA=serie)
make test                       # pruebas automáticas en QEMU sin pantalla (no toca disk.img)
```

`make` no instala si `disk.img` está en uso (QEMU abierto). QEMU para el host:
`sudo apt install qemu-system-x86` (necesita KVM con virtualización anidada). `make run` da
6 GiB a QEMU (para que las BAR PCI queden en 0x7000000000), pero el host solo gasta ~2.2 GB:
VM 768 MiB + 896 MiB de memoria para procesos, que posix_server reparte a demanda. Todo debe
caber en los 2 GiB bajos de RAM física (ver el comentario en `meta.py`).

## Arquitectura

```
                      ┌─ posix_server ── procesos creados a demanda: init, zsh, 9pd, coreutils...
consola (fbcon) ──────┤      └─ fs_ext2 ──┐
serial_virt_rx/tx     └─ CLIENT_VMM ── VM Linux (busybox + Docker + sel4-agent)
                             │  └──────────────────── disco (virtio-blk) ── part. 1: ext2 de GNU/seL4
red (virtio-net) ── net_virt ┬─ copier ──┘                                 part. 2: /var/lib/docker
                              └─ copier ── netstack (TCP/IP de FreeBSD) ── posix_server (sockets)
```

Todo es un solo sistema seL4 (`x86_64_generic_vtx`) descrito en `meta.py` (sdfgen) y compilado
por `system.mk`. Cada pieza es un componente aislado; se comunican por IPC y colas de sDDF.

- **Capa POSIX.** musl portada (`musl/port-musl.sh`): cada syscall llama a `__sel4_syscall()`
  (`runtime/syscall.c`), que la resuelve localmente (memoria, TLS, señales, hora) o la envía a
  `posix_server` por IPC. Protocolo: `include/sel4posix/protocol.h`.
- **Procesos.** Cada proceso corre en una *ranura* (hilo + espacio de direcciones + CNode) que
  `posix_server` crea a demanda con memoria de sus untypeds (`server/mem.c`, hasta 32 a la vez).
  Las regiones están en direcciones fijas (`include/sel4posix/procmem.h`), pero sus páginas se
  crean al tocarlas (fallo de página) y vuelven con exit, exec y munmap. `exec` carga ELF
  estáticos de ext2 (y `#!`); `fork` copia las páginas. También wait, tuberías, señales, `poll`.
- **Microkit propio.** La herramienta de Microkit lleva un parche (`microkit/*.patch`,
  `make microkit-tool`) que da a posix_server lo que necesita para crear procesos: untypeds,
  su CNode raíz, un pool de ASIDs, scheduling contexts y su endpoint.
- **Archivos.** `fs_ext2` (ext2 propio, bloques de 4 KiB) sobre el subsistema de bloques de sDDF.
  Protocolo VFS↔FS: `include/sel4posix/fs_protocol.h`.
- **Linux transparente.** `docker ...` y `linux comando ...` corren en la VM desde dash, con
  stdin/stdout/stderr, código de salida y pty si es interactivo. `/bin/linux` ↔ `/dev/linux` ↔
  colas compartidas con el VMM (`/dev/hvc1` en la VM) ↔ `sel4-agent`. Protocolo:
  `include/sel4posix/bridge.h`.
- **Red nativa.** `netstack` es la pila TCP/IP de FreeBSD (de F-Stack, `net/`) como componente
  propio sobre la red de sDDF: los programas usan sockets POSIX normales, que posix_server pasa
  a netstack (`include/sel4posix/net_protocol.h`). IP fija 10.0.2.16 (red de QEMU), DNS en
  `/etc/resolv.conf`. Prueba: `nettest`; `nettest servir` responde en el puerto 8090 del host.
- **Archivos compartidos.** La VM ve GNU/seL4 en `/mnt/sel4` (9P: `/sbin/9pd` ↔ `sel4-agent`).
- **Arranque.** `/sbin/init` (pid 1) ejecuta `/etc/rc` (arranca 9pd) y mantiene la shell de sesión
  de root según `/etc/passwd` (zsh). `/bin/sh` es dash, para los guiones.
- **Consola.** `fbcon` (`console/`) dibuja la terminal en la pantalla (emulador xterm-256color,
  fuente Spleen) y lee el teclado PS/2; para sDDF es el "driver serial", así que la terminal de
  posix_server no sabe si hay pantalla o puerto serie. Los mensajes del kernel van al puerto serie.
  Si cambias el tamaño de la terminal: `resize`.
- **Consola de la VM** (rara vez hace falta): `Ctrl-\ 1 Enter`; `Ctrl-\ 0 Enter` vuelve a dash.

## Compilar programas para GNU/seL4

`build/sysroot/bin/sel4-cc` es el compilador (como `musl-gcc`):
`./configure --host=x86_64-linux-musl CC=.../sel4-cc`. El sysroot no lleva headers del kernel
Linux: el triple `x86_64-linux-musl` solo indica la ABI de syscalls que imita posix_server.
Programas propios: `apps/<nombre>/*.c` → `/bin/<nombre>` (o `/sbin` si está en `SBIN_APPS`).
Ports de terceros: `ports/<programa>/`.

## Directorios de `proyectos/gnu-sel4`

| Directorio | Contenido |
|---|---|
| `server/` | posix_server: `posix_server.c` (despacho), `proc.c`, `mem.c` (memoria y ranuras), `vfs.c`, `tty.c`, `clock.c`, `bridge.c` |
| `fs/ext2/` | `fs_ext2.c` |
| `console/` | fbcon: consola gráfica (`fbcon.c`: pantalla y teclado; `term.c`: emulador de terminal) |
| `net/` | netstack: pila TCP/IP de FreeBSD (`netstack.c`: sDDF y llamadas; `host.c`: lo que F-Stack espera del SO) |
| `runtime/`, `musl/`, `tools/` | runtime de los programas, parche de musl, `sel4-cc` |
| `microkit/` | parche a la herramienta de Microkit y `build-tool.sh` |
| `vm/` | VMM de la VM, `vm/agent/` (sel4-agent), `vm/rootfs/` (busybox + Docker) |
| `apps/` | init, 9pd, linux, resize, nettest, proctest y archivos (pruebas) |
| `ports/` | dash, coreutils, ncurses, zsh, vim y `fstack` (compila la parte de FreeBSD de F-Stack) |
| `rootfs/` | contenido inicial de la partición ext2 (`/etc/rc`, `/etc/profile`, ...) |

Revisar el disco desde el host: `../../dev.sh e2fsck -fn "disk.img?offset=1048576"`.
