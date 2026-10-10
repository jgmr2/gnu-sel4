# GNU/seL4 — work plan

Status: `[x]` done · `[~]` in progress · `[ ]` pending

## Done

- [x] Environment: Microkit SDK 2.3.1, builds in Docker (`dev.sh`, LLVM 18), QEMU+KVM on the host
- [x] POSIX layer: musl 1.2.6 ported (syscalls → seL4 IPC), runtime, `sel4-cc`
- [x] `posix_server`: processes in slots (exec, fork, wait, signals, groups), tty, VFS, pipes,
      `poll`/`select`, clock (RTC + TSC) and timer
- [x] `fs_ext2`: our own ext2 on sDDF, verified with `e2fsck`
- [x] dash 0.5.13.5 (`/bin/sh`); zsh as the login shell (per /etc/passwd); `/sbin/init` + `/etc/rc`
- [x] Linux VM with Docker (libvmm) on the same seL4; 2-partition disk; networking for the VM
- [x] WSL-style transparent Linux: `docker`, `linux command`, interactive pty
- [x] Files shared with the VM: `/mnt/sel4` over 9P (`9pd`)
- [x] Repository cleanup (old docker-vm, demos, backups, example builds: ~1.9 GB)
- [x] **GNU coreutils 9.12**: 101 programs in `/bin` (multi-call binary), no Linux headers;
      `uname -o` = GNU/seL4. Added to the runtime: statx, utimensat, preadv/pwritev, sysinfo,
      prctl, sched_getaffinity, fadvise
- [x] `nice`/`renice` (getpriority/setpriority) and a provisional `clear`

## In order

1. [x] Repository cleanup
2. [x] GNU coreutils (replaced `utiles`)
3. [x] **zsh and vim**: ncurses 6.6 (clear, tput, reset… + terminfo), zsh 5.9.2 (with /etc/zshrc),
       vim 9.2 (trimmed runtime). Added: mmap allocator with real munmap, setitimer/alarm
       (SIGALRM), chown/fchown/lchown/fchownat
   - [x] Host RAM 4.3 → 2.2 GB: seL4 zeroes each untyped block in full when it is first used,
         so everything must fit in the low 2 GiB. Slots: image 8 MiB, heap 4, mmap 64; 768 MiB
         VM with a trimmed initramfs (337 → 252 MiB: no useless modules, no `ctr`)
4. [ ] **Dynamic user-space memory manager**
   - [x] Microkit 2.3.1 built from source (`make microkit-tool`, patch in
         `proyectos/gnu-sel4/microkit/`): `<cap_untyped>`, `<cap_cnode>`, `<cap_asid_pool>`,
         `<cap_sched_pool>` and `<cap_endpoint>` in `<cspace>`
   - [x] On-demand processes (`server/mem.c`): posix_server creates the TCB, address space,
         CNode and notification from its untyped; pages on page fault; exit/exec/munmap
         return them. No slot PDs in meta.py. Idle process memory: 924 MiB reserved → ~4 MiB
         used; up to 32 processes; provisional `free`
   - [ ] Copy-on-write fork (today it copies every page)
   - [ ] More than 32 processes (64-bit masks in vfs/tty/bridge, or wait lists)
   - [ ] Large pages (2 MiB) for large regions; free unused page tables
5. [x] **Native networking with the FreeBSD stack**
   - [x] `netstack` component (`net/`): the FreeBSD TCP/IP stack from F-Stack 1.25
         (`third_party/f-stack`, only `lib/` and `freebsd/`, no DPDK) on sDDF networking, with
         its own copier; musl inside the PD as its libc. TCP, UDP, IPv4/IPv6, ARP, routes
   - [x] POSIX sockets: posix_server (`OF_SOCKET` in vfs.c) calls netstack over PPC
         (`include/sel4posix/net_protocol.h`); blocking with POSIX_RETRY and kqueue wake-ups.
         socket, bind, connect, listen, accept(4), send/recv(from/msg), shutdown, sockopts,
         get{sock,peer}name, poll/select, FIONBIO/FIONREAD, O_NONBLOCK. DNS via /etc/resolv.conf
   - [x] `nettest` (DNS + HTTP to the internet; `nettest servir` from the host: port 8090)
   - [x] DHCP (`net/dhcp.c`): frame-level client in front of the stack; the FreeBSD interface
         is created with the lease (INIT-REBOOT with the preferred IP, DISCOVER, renewal at T1).
         posix_server rewrites /etc/resolv.conf with the received DNS servers; `/proc/net/dhcp`.
         Fixed MACs (they used to change on every build). `make NETSTACK_IP_PREFERIDA=` = no preference
   - [x] Configuration: `red` (status, `red dhcp`, `red fija IP/P puerta G dns D`, `red aplicar`
         with /etc/red.conf at boot); netstack changes address and route on the fly
   - [x] Loopback 127.0.0.1 (FreeBSD's deferred queue); AF_UNIX STREAM and DGRAM
   - [x] Tools: curl with HTTPS (mbedTLS 3.6 + Mozilla certificates; getrandom and
         /dev/urandom via RDRAND; sel4-cc links compiler-rt), socat + `nc`, dropbear ssh/scp
         client (tested with an ed25519 key against sshd in a container). Hard links in fs_ext2
   - [x] Performance measured: native download ~38 MB/s, equal to or better than the VM
         (~31 MB/s): the limit today is QEMU user networking, not the process -> server ->
         netstack copies
   - [ ] ssh server (dropbear): needs pseudo-terminals (/dev/ptmx), which foot also needs
   - [ ] Direct networking between native and the VM (today it goes around via the host):
         sDDF vswitch or virtio-net between netstack and the VM (see item 8)
6. [ ] **Robustness** (next: it protects everything else)
   - [~] Version control: git repository (`.gitignore`, `DEPENDENCIES.md`); missing: the push to
         github.com:jgmr2/gnu-sel4 (register the SSH key) and a script that fetches the dependencies
   - [x] `make test` (`tools/prueba.py`): copies the system partition, creates /etc/prueba, boots
         headless QEMU; /etc/pruebas.sh runs the tests and writes to /dev/kmsg (to the serial
         port). 15 tests in ~15 s. `make test SIN_RED=1` without internet
   - [ ] Named pipes (`mkfifo`): a FIFO in ext2 that the server treats as a pipe
         (s6 uses them to control services)
   - [ ] s6 + s6-rc as the supervisor (skarnet, ISC; maintained, in C; chosen over runit, which
         has not been maintained since 2014, and has no dependencies): skalibs + s6 + s6-rc
         (+ execline). `s6-svscan` as PID 1 or started by `/sbin/init`; services: 9pd,
         login + zsh; later dhcp, sshd, sway. s6-rc: dependencies ("sshd after the network")
         and one-shot tasks (fsck, time, network). `s6-svc`/`s6-rc` to inspect, stop and
         restart. skalibs without signalfd until Phase 1b (it uses its fallback)
   - [ ] Real `poweroff`/`reboot`: stop services, `sync`, shut down the VM, power off the machine (ACPI)
   - [ ] ext2 fsck at boot if the disk was left dirty (ext2 state flag)
7. [~] **Graphical desktop: sway + Wayland**
   - [x] vim with syntax highlighting (syncolor.vim and colors/lists were missing from the runtime)
   - [x] Phase 0: `fbcon` graphical console (`console/`): QEMU bochs-display at 1280x800 (160x50),
         PS/2 keyboard (i8042, US), our own xterm-256color terminal emulator (UTF-8, 256 and
         24-bit colour, alternate screen, scroll regions, DEC graphics, DA/CPR replies), Spleen
         8x16 font. It is sDDF's "serial driver": nothing else changed. Output 17x faster than
         the serial port (seq 20000: 4.25 s -> 0.24 s). `make CONSOLA=serie` goes back to the UART
   - [ ] Phase 0a: UEFI GOP framebuffer (laptop); 2x font scaling for HiDPI screens;
         scrollback with Shift+PgUp
   - [ ] Phase 0b: PS/2 mouse; Latin American keyboard layout; virtual consoles
   - [x] Phase 1: what Wayland needs from POSIX (`fase1test`, all passing, no leaks):
         epoll (in the runtime, on top of poll), eventfd and timerfd (server); AF_UNIX STREAM
         sockets with bind/listen/accept/connect, socketpair, SCM_RIGHTS and SO_PEERCRED
         (server); memfd_create + ftruncate + mmap MAP_SHARED across processes (mem.c: copies
         of each page's capability; fork shares them); file mmap (private copy); flock
   - [ ] Phase 1b: signalfd, shm_open (/dev/shm), AF_UNIX SEQPACKET, nested epoll in poll
   - [ ] Phase 2: libraries: libwayland, wayland-protocols, libffi, libxkbcommon (+ keymaps),
         pixman, json-c, pcre2; cairo + pango (glib, harfbuzz, freetype, fontconfig).
         meson/ninja in the build image
   - [ ] Phase 3: wlroots with our own seL4 backend (framebuffer + input; pixman renderer)
         and unmodified sway
   - [ ] Phase 4: foot as the native terminal; Linux VM windows through waypipe (like WSLg);
         per-origin border colours (native / VM), Qubes-style
8. [ ] **Linux VM integration** (the VM runs Linux at native speed with VT-x; what is slow is
       the current bridge: serial-port-style queues with byte-by-byte copies)
   - [ ] vsock (AF_VSOCK): POSIX sockets between native and VM processes (virtio-vsock in
         libvmm, AF_VSOCK in posix_server); `docker`/`linux` over vsock instead of the current bridge
   - [ ] Fast shared files: 9P or virtio-fs over shared-memory queues
   - [ ] Dynamic VM memory: the VM sees plenty of RAM, but each page is created on first use
         (EPT fault in libvmm -> page from the pool) and goes back to the pool when Linux frees
         it (virtio-balloon with "free page reporting"). The VM RAM window in the VMM also on
         demand. A cap per VM and a reserve for native (so a container cannot eat the desktop)
   - [ ] Memory server: move mem.c out of posix_server into a component that hands pages to
         posix_server and the VMM (a single pool); Microkit: capability to the VM's EPT
   - [ ] The VM's system on disk instead of an initramfs in RAM (252 MB fixed today)
9. [ ] **Journal in fs_ext2** (towards ext3/ext4)
10. [ ] **Licensing**: choose a license for our own code, SPDX headers, `THIRD_PARTY.md`
11. [ ] **Booting on real hardware (Dell Latitude 7390 2-in-1, UEFI)**
   - [ ] Live ISO: GRUB (multiboot2) loads seL4 + the system + an ext2 disk into RAM
   - [ ] GOP framebuffer (address and format from multiboot2) for the graphical console; RAM disk
   - [ ] Persistent disk with the sDDF NVMe driver
   - [ ] Wi-Fi (Intel 8265), USB, audio: passthrough (VT-d) to the Linux VM as a "driver VM"
   - [ ] Touchpad/touchscreen (I²C-HID)
   - [ ] Native networking through the driver VM: virtio-net between netstack and the VM (the
         current serial-style bridge is not fit for data)
   - [ ] Alternative to the driver VM for Wi-Fi: the Linux driver inside a component
         (LKL or Genode-style DDE), no VM
   - [ ] Multicore (seL4 runs on a single core today; the laptop has 4)
   - [ ] Release build (kernel and system without debugging)

## Critical gaps (review of 2026-10-09)

What matters most for daily use, beyond the items above:

- [ ] **Threads (pthreads)**: several seL4 TCBs in one address space. Blocks glib (pango,
      sway), browsers and almost any large program -> before cairo/pango (Phase 2)
- [ ] **More than 32 processes** and larger server tables (today: 128 open files,
      32 pipes, 64 sockets): a desktop exceeds them
- [ ] **posix_server is a single point of failure**: if it goes down, all of native goes down;
      split it into servers (processes/memory, files, network, tty) in the long run
- [ ] **Users and permissions**: today everything is root and permissions are not checked; real login
- [ ] **Installing software without rebuilding the image** (packages or updates); a bigger ext2
      partition (64 MB is no longer enough for Phase 2)
- [ ] Everyday tools: grep, sed, findutils, diffutils, less, tar, gzip (curl, ssh,
      socat/nc are done)

## Known issues (unscheduled)

- [ ] Asynchronous signals to processes that make no syscalls (pure loops); `Ctrl-Z` / stopping processes
- [ ] `Ctrl-C` in the middle of dash's prompt is processed with the next line
- [ ] Our own triple (`x86_64-sel4-musl`) instead of `x86_64-linux-musl` (today programs see `__linux__`)
- [ ] Shared writable `mmap` of files (today: private copy, or shared read-only)
- [ ] Process owner (uid/gid) when creating files (always root today)
- [ ] Pseudo-terminals (/dev/ptmx, openpty): sshd, foot, tmux/screen
- [x] Real terminal size: `resize` (asks the terminal with CSI 6n) at login,
      TIOCSWINSZ + SIGWINCH. If you resize the window: `resize`
- [ ] `timer_create` (POSIX timers) and threads (pthreads)
- [x] Virtual `/proc` (`server/procfs.c`) + procps-ng 4.0.7: ps, top, free, uptime, pgrep, pkill,
      pidof, watch, vmstat, pwdx, tload. Per-process CPU from seL4 scheduling contexts
- [ ] /proc: real loadavg, /proc/<pid>/maps and fd, named tty (today "?")
- [ ] Make `nice` change the real seL4 priority (today the value is only stored)
- [ ] Port to ARM64 / Raspberry Pi 4 (sDDF lacks an SD driver)
