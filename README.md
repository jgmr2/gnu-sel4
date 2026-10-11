# GNU/seL4

A Unix-like operating system built on the seL4 microkernel: GNU and POSIX programs run
natively on seL4, and a Linux VM runs next to them for Docker and anything else that needs
Linux, integrated transparently (like WSL2). Work plan and status: [`TODO.md`](TODO.md).

## Goal

A system for everyday use on a real laptop (the target is a Dell Latitude 7390 2-in-1): a terminal with
zsh, vim and the GNU tools, networking, an ssh client and server, a Wayland desktop (sway +
foot), and Docker for everything else. All of it on top of a formally verified microkernel,
where every part of the system is an isolated component that only has the capabilities it
was given.

The path there, in order: robustness (version control, tests, a service supervisor, clean
shutdown, fsck) → the desktop → deeper integration with the Linux VM → a journaling file
system → real hardware. Hardware comes last on purpose: first the system has to be worth
booting.

## Philosophy

- **Minimal layers.** No Linux distribution underneath and no compatibility kernel: seL4,
  a small set of our own components, and a ported libc. Every layer must justify itself;
  when something can be done in fewer pieces, it is.
- **Native first, Linux as a guest.** Programs are compiled for GNU/seL4 and run directly on
  it. Linux is not the foundation but a guest in a VM, used for what is not worth porting
  (Docker, drivers on real hardware). Crossing to it should feel seamless: `docker run ...`
  from zsh just works, with stdin/stdout, exit codes and a pty.
- **No Linux dependencies in the native system.** The sysroot carries no Linux kernel
  headers. `x86_64-linux-musl` only names the syscall ABI that posix_server imitates, so that
  unmodified upstream software builds with `./configure`.
- **Isolation by capabilities.** The console, file system, network stack, POSIX server and
  VMM are separate seL4 protection domains that talk over IPC and shared queues (sDDF).
  A component can only touch what it was explicitly given.
- **Upstream software, unmodified where possible.** dash, zsh, GNU coreutils, vim, procps,
  curl, dropbear and the FreeBSD TCP/IP stack are ported, not rewritten. When something is
  missing, the fix goes into the POSIX layer, not into the program.
- **Small, maintained dependencies.** Prefer well-maintained, small C projects with permissive
  licenses (e.g. s6 over runit, mbedTLS over OpenSSL). Exact versions are pinned in
  [`DEPENDENCIES.md`](DEPENDENCIES.md).
- **Resources on demand.** Processes, pages and (planned) VM memory are created when they are
  first used and returned when freed, instead of being reserved up front.
- **Measure and test.** Every feature gets a test in `make test`, which boots the real system
  in a headless QEMU; performance claims are measured, not assumed.

## Layout

```
deps.lock, fetch-deps.sh   every dependency with its exact version and hash, and the script that fetches them
sdk/                 Microkit SDK 2.3.1 (seL4 kernel + libmicrokit)             (fetched)
third_party/         musl, F-Stack, GNU tools, zsh, vim, curl...; microkit/ = the Microkit tool's
                     source with our patches                                     (fetched)
libvmm/              libvmm (seL4 VMM) with sDDF in libvmm/dep/sddf (drivers and virtualisers) (fetched)
projects/gnu-sel4/   the system
Dockerfile, dev.sh, docker-compose.yml   build environment (clang/LLVM 18, make, dtc, iasl, sdfgen, e2fsprogs)
```

## Usage

The only requirement on the host is Docker; QEMU with KVM (nested virtualisation) runs the
system: `sudo apt install qemu-system-x86`.

**With Docker Compose** (simplest: the image `jgmr2/gnu-sel4-dev` from Docker Hub carries every
dependency, already checked, and the patched Microkit tool, ~3 GB; if it cannot be pulled, it is
built locally in about 7 minutes):

```sh
git clone https://github.com/jgmr2/gnu-sel4 && cd gnu-sel4
docker compose run --rm vm-rootfs   # (once) kernel + initramfs for the Linux VM
docker compose run --rm build       # build everything and install it into projects/gnu-sel4/disk.img
docker compose run --rm test        # automated tests in headless QEMU
cd projects/gnu-sel4 && make run    # boot it (on the host: QEMU opens a window)
```

**With the dependencies on the host** (what development uses; same result):

```sh
./fetch-deps.sh                 # download and verify the dependencies (deps.lock) next to the repository
./dev.sh build                  # build the Docker build image (once)
cd projects/gnu-sel4
make microkit-tool              # (host, once) the Microkit tool with our patches (Rust in Docker)
make vm-rootfs                  # (host, once) kernel + initramfs for the VM
../../dev.sh make               # build everything and install it into disk.img
make run                        # QEMU + KVM with a graphical console (window); quit: Ctrl-a x in the terminal
make run CONSOLE=serial         # everything over the serial port (also build with CONSOLE=serial)
make test                       # automated tests in headless QEMU (does not touch disk.img)
```

`make` refuses to install while `disk.img` is in use (QEMU running). QEMU on the host:
`sudo apt install qemu-system-x86` (needs KVM with nested virtualisation). `make run` gives
QEMU 6 GiB (so the PCI BARs land at 0x7000000000), but the host only spends ~2.2 GB:
768 MiB for the VM + 896 MiB of process memory, which posix_server hands out on demand.
Everything must fit in the low 2 GiB of physical RAM (see the comment in `meta.py`).

## Architecture

```
                      ┌─ posix_server ── processes created on demand: init, zsh, 9pd, coreutils...
console (fbcon) ──────┤      └─ fs_ext2 ──┐
serial_virt_rx/tx     └─ CLIENT_VMM ── Linux VM (busybox + Docker + sel4-agent)
                             │  └──────────────────── disk (virtio-blk) ── part. 1: GNU/seL4 ext2
net (virtio-net) ── net_virt ┬─ copier ──┘                                 part. 2: /var/lib/docker
                              └─ copier ── netstack (FreeBSD TCP/IP) ── posix_server (sockets)
```

The whole thing is a single seL4 system (`x86_64_generic_vtx`) described in `meta.py`
(sdfgen) and built by `system.mk`. Each piece is an isolated component; they communicate
over IPC and sDDF queues.

- **POSIX layer.** A port of musl (`musl/port-musl.sh`): every syscall calls
  `__sel4_syscall()` (`runtime/syscall.c`), which handles it locally (memory, TLS, signals,
  time) or sends it to `posix_server` over IPC. Protocol: `include/sel4posix/protocol.h`.
- **Processes.** Each process runs in a *slot* (thread + address space + CNode) that
  `posix_server` creates on demand from its untyped memory (`server/mem.c`, up to 32 at a
  time). Regions sit at fixed addresses (`include/sel4posix/procmem.h`), but their pages are
  created on first touch (page fault) and returned on exit, exec and munmap. `exec` loads
  static ELF binaries from ext2 (and `#!` scripts); `fork` copies the pages. Also wait,
  pipes, signals, `poll`.
- **Our own Microkit.** The Microkit tool carries a patch (`microkit/*.patch`,
  `make microkit-tool`) that gives posix_server what it needs to create processes: untypeds,
  its root CNode, an ASID pool, scheduling contexts and its endpoint.
- **Files.** `fs_ext2` (our own ext2, 4 KiB blocks) on top of the sDDF block subsystem.
  VFS↔FS protocol: `include/sel4posix/fs_protocol.h`.
- **Transparent Linux.** `docker ...` and `linux command ...` run in the VM from the native
  shell, with stdin/stdout/stderr, exit code and a pty when interactive. `/bin/linux` ↔
  `/dev/linux` ↔ queues shared with the VMM (`/dev/hvc1` in the VM) ↔ `sel4-agent`.
  Protocol: `include/sel4posix/bridge.h`.
- **Native networking.** `netstack` is the FreeBSD TCP/IP stack (from F-Stack, `net/`) as a
  component of its own on top of sDDF networking: programs use ordinary POSIX sockets, which
  posix_server forwards to netstack (`include/sel4posix/net_protocol.h`). Address via DHCP
  (under QEMU it asks for 10.0.2.16, the target of the `make run` port forwards) or static
  with `netcfg`; DNS servers go to `/etc/resolv.conf`. Status: `netcfg` or `cat /proc/net/dhcp`.
  Tools: curl (HTTPS), socat/nc, ssh/scp (dropbear client), `nettest`.
- **Shared files.** The VM sees GNU/seL4 at `/mnt/sel4` (9P: `/sbin/9pd` ↔ `sel4-agent`).
- **Boot.** `/sbin/init` (pid 1) runs `/etc/rc` (network, 9pd) and keeps root's login shell
  alive as given in `/etc/passwd` (zsh). `/bin/sh` is dash, for scripts.
- **Console.** `fbcon` (`console/`) draws the terminal on screen (xterm-256color emulator,
  Spleen font) and reads the PS/2 keyboard; to sDDF it is the "serial driver", so
  posix_server's terminal does not know whether there is a screen or a serial port. Kernel
  messages go to the serial port. If the terminal size changes: `resize`.
- **VM console** (rarely needed): `Ctrl-\ 1 Enter`; `Ctrl-\ 0 Enter` returns to GNU/seL4.

## Building programs for GNU/seL4

`build/sysroot/bin/sel4-cc` is the compiler (like `musl-gcc`):
`./configure --host=x86_64-linux-musl CC=.../sel4-cc`.
Our own programs: `apps/<name>/*.c` → `/bin/<name>` (or `/sbin` if listed in `SBIN_APPS`).
Third-party ports: `ports/<program>/`.

## Directories in `projects/gnu-sel4`

| Directory | Contents |
|---|---|
| `server/` | posix_server: `posix_server.c` (dispatch), `proc.c`, `mem.c` (memory and slots), `vfs.c`, `tty.c`, `clock.c`, `bridge.c`, `procfs.c` |
| `fs/ext2/` | `fs_ext2.c` |
| `console/` | fbcon: graphical console (`fbcon.c`: screen and keyboard; `term.c`: terminal emulator) |
| `net/` | netstack: FreeBSD TCP/IP stack (`netstack.c`: sDDF and calls; `dhcp.c`: DHCP client; `host.c`: what F-Stack expects from the OS) |
| `runtime/`, `musl/`, `tools/` | program runtime, musl patch, `sel4-cc`, `make test` (`test.py`) |
| `microkit/` | patch to the Microkit tool and `build-tool.sh` |
| `vm/` | the VM's VMM, `vm/agent/` (sel4-agent), `vm/rootfs/` (busybox + Docker) |
| `apps/` | init, 9pd, linux, netcfg, resize, nettest and tests (proctest, phase1test, filetest) |
| `ports/` | dash, coreutils, ncurses, zsh, vim, procps, curl, socat, dropbear, `gnu` (grep, sed, findutils, diffutils, tar, gzip, less) and `fstack` (builds F-Stack's FreeBSD part) |
| `rootfs/` | initial contents of the ext2 partition (`/etc/rc`, `/etc/profile`, ...) |

Check the disk from the host: `../../dev.sh e2fsck -fn "disk.img?offset=1048576"`.

## License

The code written for GNU/seL4 is under the [BSD-2-Clause](LICENSE) license: anyone may use,
change and redistribute it, keeping the copyright notice. Everything else is the work of other
projects, each under its own license; [`THIRD_PARTY.md`](THIRD_PARTY.md) credits all of them, and
the installed system carries their license texts in `/usr/share/licenses/`.
