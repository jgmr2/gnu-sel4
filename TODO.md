# GNU/seL4 — work plan

Status: `[x]` done · `[~]` in progress · `[ ]` pending

## Done

- [x] Environment: Microkit SDK 2.3.1, builds in Docker (`dev.sh`, LLVM 18), QEMU+KVM on the host
- [x] POSIX layer: musl 1.2.6 ported (syscalls → seL4 IPC), runtime, `sel4-cc`
- [x] `posix_server`: processes in slots (exec, fork, wait, signals, groups), tty, VFS, pipes,
      `poll`/`select`, clock (RTC + TSC) and timer
- [x] `fs_ext2`: our own ext2 on sDDF, verified with `e2fsck`; hard links
- [x] dash 0.5.13.5 (`/bin/sh`); zsh as the login shell (per /etc/passwd); `/sbin/init` + `/etc/rc`
- [x] Linux VM with Docker (libvmm) on the same seL4; 2-partition disk; networking for the VM
- [x] WSL-style transparent Linux: `docker`, `linux command`, interactive pty
- [x] Files shared with the VM: `/mnt/sel4` over 9P (`9pd`)
- [x] Repository cleanup (old docker-vm, demos, backups, example builds: ~1.9 GB); everything in English
- [x] **GNU coreutils 9.12**: 101 programs in `/bin` (multi-call binary), no Linux headers;
      `uname -o` = GNU/seL4. Added to the runtime: statx, utimensat, preadv/pwritev, sysinfo,
      prctl, sched_getaffinity, fadvise
- [x] `nice`/`renice` (getpriority/setpriority) and a provisional `clear`
- [x] **zsh and vim**: ncurses 6.6 (clear, tput, reset… + terminfo), zsh 5.9.2 (with /etc/zshrc),
      vim 9.2 (trimmed runtime, syntax highlighting). Added: mmap allocator with real munmap,
      setitimer/alarm (SIGALRM), chown/fchown/lchown/fchownat
- [x] Host RAM 4.3 → 2.2 GB: seL4 zeroes each untyped block in full when it is first used,
      so everything must fit in the low 2 GiB. 768 MiB VM with a trimmed initramfs
      (337 → 252 MiB: no useless modules, no `ctr`)
- [x] **Dynamic user-space memory manager** (first part)
  - Microkit 2.3.1 built from source (`make microkit-tool`, patches in
    `projects/gnu-sel4/microkit/`): `<cap_untyped>`, `<cap_cnode>`, `<cap_asid_pool>`,
    `<cap_sched_pool>` and `<cap_endpoint>` in `<cspace>`
  - On-demand processes (`server/mem.c`): posix_server creates the TCB, address space,
    CNode and notification from its untyped; pages on page fault; exit/exec/munmap return
    them. No slot PDs in meta.py. Idle process memory: 924 MiB reserved → ~4 MiB used;
    up to 32 processes
- [x] Virtual `/proc` (`server/procfs.c`) + procps-ng 4.0.7: ps, top, free, uptime, pgrep, pkill,
      pidof, watch, vmstat, pwdx, tload. Per-process CPU from seL4 scheduling contexts
- [x] Real terminal size: `resize` (asks the terminal with CSI 6n) at login, TIOCSWINSZ + SIGWINCH
- [x] **Native networking with the FreeBSD stack**
  - `netstack` component (`net/`): the FreeBSD TCP/IP stack from F-Stack 1.25
    (`third_party/f-stack`, only `lib/` and `freebsd/`, no DPDK) on sDDF networking, with its
    own copier; musl inside the PD as its libc. TCP, UDP, IPv4/IPv6, ARP, routes
  - POSIX sockets: posix_server (`OF_SOCKET` in vfs.c) calls netstack over PPC
    (`include/sel4posix/net_protocol.h`); blocking with POSIX_RETRY and kqueue wake-ups.
    socket, bind, connect, listen, accept(4), send/recv(from/msg), shutdown, sockopts,
    get{sock,peer}name, poll/select, FIONBIO/FIONREAD, O_NONBLOCK. DNS via /etc/resolv.conf
  - DHCP (`net/dhcp.c`): frame-level client in front of the stack (INIT-REBOOT with the preferred
    IP, DISCOVER, renewal at T1); posix_server rewrites /etc/resolv.conf; `/proc/net/dhcp`.
    Fixed MACs. `make NETSTACK_PREFERRED_IP=` = no preference
  - Configuration: `netcfg` (status, `netcfg dhcp`, `netcfg static IP/P gateway G dns D`,
    `netcfg apply` with /etc/netcfg.conf at boot); address and route change on the fly
  - Loopback 127.0.0.1 (FreeBSD's deferred queue); AF_UNIX STREAM and DGRAM
  - Tools: `nettest` (`nettest serve` from the host: port 8090), curl with HTTPS (mbedTLS 3.6 +
    Mozilla certificates; getrandom and /dev/urandom via RDRAND; sel4-cc links compiler-rt),
    socat + `nc`, dropbear ssh/scp client (tested with an ed25519 key)
  - Performance: native download ~38 MB/s, equal to or better than the VM (~31 MB/s): the limit
    is QEMU user networking, not the process -> server -> netstack copies
- [x] **Graphical console** (desktop Phase 0): `fbcon` (`console/`): QEMU bochs-display at
      1280x800 (160x50), PS/2 keyboard (i8042, US), our own xterm-256color terminal emulator
      (UTF-8, 256 and 24-bit colour, alternate screen, scroll regions, DEC graphics, DA/CPR
      replies), Spleen 8x16 font. It is sDDF's "serial driver": nothing else changed. Output 17x
      faster than the serial port. `make CONSOLE=serial` goes back to the UART
- [x] **What Wayland needs from POSIX** (desktop Phase 1, `phase1test`, all passing, no leaks):
      epoll (in the runtime, on top of poll), eventfd and timerfd (server); AF_UNIX STREAM sockets
      with bind/listen/accept/connect, socketpair, SCM_RIGHTS and SO_PEERCRED (server);
      memfd_create + ftruncate + mmap MAP_SHARED across processes (mem.c: copies of each page's
      capability; fork shares them); file mmap (private copy); flock
- [x] Version control: git repository on github.com/jgmr2/gnu-sel4 (`.gitignore`, `DEPENDENCIES.md`)
- [x] `make test` (`tools/test.py`): copies the system partition, creates /etc/test, boots headless
      QEMU; /etc/tests.sh runs the tests and writes to /dev/kmsg (to the serial port). 16 tests in
      ~20 s (including the bridge to the Linux VM). `make test NO_NET=1` without internet

## Next, ordered by cost/benefit

Cost: **S** = one session or less · **M** = a few sessions · **L** = weeks. Earlier items are
cheap and unlock a lot; later ones are expensive or only pay off once the earlier ones exist.
Real hardware stays last on purpose: first the system has to be worth booting.

1. [ ] **Everyday tools** (S): GNU grep, sed, findutils (find, xargs), diffutils, less, tar, gzip.
       Same recipe as coreutils (`./configure` + gnulib with `sel4-cc`). Without them any real
       session hits a wall; s6-rc scripts and the desktop builds need them too
2. [ ] **Licensing** (S): choose a license for our own code, SPDX headers, `THIRD_PARTY.md`.
       The repository is public: without a license nobody may legally reuse or contribute
3. [ ] **Fetch-dependencies script** (S): downloads and checks everything in `DEPENDENCIES.md`, so
       the repository builds on another machine (also the first step of a CI)
4. [ ] **Supervision: `mkfifo` + s6 + s6-rc** (M)
   - [ ] Named pipes (`mkfifo`): a FIFO in ext2 that the server treats as a pipe
         (s6 uses them to control services)
   - [ ] s6 + s6-rc (skarnet, ISC; maintained, in C; chosen over runit, which has not been
         maintained since 2014): skalibs + s6 + s6-rc (+ execline). `s6-svscan` as PID 1 or
         started by `/sbin/init`; services: 9pd, login + zsh; later dhcp, sshd, sway. s6-rc:
         dependencies ("sshd after the network") and one-shot tasks (fsck, time, network).
         skalibs without signalfd until signalfd exists (it uses its fallback).
         Crashed services come back on their own: the first step towards restarting the userland
5. [ ] **Clean shutdown and disk safety** (S–M)
   - [ ] Real `poweroff`/`reboot`: stop services, `sync`, shut down the VM, power off (ACPI)
   - [ ] ext2 fsck at boot if the disk was left dirty (ext2 state flag)
6. [ ] **Pseudo-terminals** (M): `/dev/ptmx`, openpty. Unlocks the ssh server (dropbear),
       tmux/screen and later foot
7. [ ] **Room to grow** (M)
   - [ ] A bigger ext2 partition (64 MB is not enough for the desktop libraries)
   - [ ] Installing software without rebuilding the image (packages or updates)
   - [ ] More than 32 processes and larger server tables (today: 128 open files, 32 pipes,
         64 sockets): a desktop exceeds them (64-bit masks in vfs/tty/bridge, or wait lists)
8. [ ] **Job control and signals** (M): asynchronous signals to processes that make no syscalls
       (pure loops); `Ctrl-Z` / stopping processes; `Ctrl-C` in the middle of dash's prompt is
       processed with the next line
9. [ ] **Threads: pthreads and `timer_create`** (L): several seL4 TCBs in one address space.
       Blocks glib (pango, sway), browsers, Node.js and almost any large program
10. [ ] **Graphical desktop: sway + Wayland** (L), on top of items 6, 7 and 9
   - [ ] Phase 1b: signalfd, shm_open (/dev/shm), AF_UNIX SEQPACKET, nested epoll in poll
   - [ ] Phase 0b: PS/2 mouse; Latin American keyboard layout; virtual consoles
   - [ ] Phase 2: libraries: libwayland, wayland-protocols, libffi, libxkbcommon (+ keymaps),
         pixman, json-c, pcre2; cairo + pango (glib, harfbuzz, freetype, fontconfig).
         meson/ninja in the build image
   - [ ] Phase 3: wlroots with our own seL4 backend (framebuffer + input; pixman renderer)
         and unmodified sway
   - [ ] Phase 4: foot as the native terminal; Linux VM windows through waypipe (like WSLg);
         per-origin border colours (native / VM), Qubes-style
11. [ ] **Performance** (M): measure first, then optimise what the numbers point at
   - [ ] Measurements: TSC counters for fork, exec, exit, page faults, PPCs and copies, visible
         in /proc (numbers before any optimisation)
   - [ ] Copy-on-write fork: today `mem_fork` copies every page, and fork + exec (every command
         the shell starts) copies the whole shell only to throw it away at the exec
   - [ ] Asynchronous I/O in fs_ext2: today it serves one disk request at a time and busy-waits
         for the reply (`blk_io`), so nothing else touching files makes progress. A
         submission/completion ring between posix_server and fs_ext2 (the sDDF model, the useful
         part of the io_uring idea): several requests in flight, sleep until the disk notifies
   - [ ] Zero copy where it pays off: shared `mmap` of files mapping the fs_ext2 cache pages
         into the process (today: private copy, or shared read-only); large read/write
         (>= 64 KiB) passing page capabilities instead of copying. Small transfers keep copying:
         mapping + unmapping costs more than copying 4 KiB
   - [ ] Large pages (2 MiB) for large regions; free unused page tables
12. [ ] **Linux VM integration** (M–L; the VM runs Linux at native speed with VT-x; what is slow
        is the current bridge: serial-port-style queues with byte-by-byte copies)
   - [ ] vsock (AF_VSOCK): POSIX sockets between native and VM processes (virtio-vsock in
         libvmm, AF_VSOCK in posix_server); `docker`/`linux` over vsock instead of the current
         bridge. virtio rings are zero copy: the biggest copy saving in the whole system
   - [ ] Direct networking between native and the VM (today it goes around via the host):
         sDDF vswitch or virtio-net between netstack and the VM
   - [ ] Fast shared files: 9P or virtio-fs over shared-memory queues
   - [ ] Dynamic VM memory: the VM sees plenty of RAM, but each page is created on first use
         (EPT fault in libvmm -> page from the pool) and goes back to the pool when Linux frees
         it (virtio-balloon with "free page reporting"). A cap per VM and a reserve for native
   - [ ] Memory server: move mem.c out of posix_server into a component that hands pages to
         posix_server and the VMM (a single pool); Microkit: capability to the VM's EPT
   - [ ] The VM's system on disk instead of an initramfs in RAM (252 MB fixed today)
13. [ ] **Self-hosted development loop** (L): develop GNU/seL4 from inside GNU/seL4,
        autonomously: edit → build → stage → restart the whole userland (not the kernel) →
        read logs and behaviour → commit if it is good
   - [ ] First step: Claude Code + toolchain in the Linux VM, working on the tree through
         `/mnt/sel4` (needs dynamic VM memory: the build does not fit in 768 MiB)
   - [ ] Userland restart: a monitor component with authority over the other PDs that stops
         them, loads new images from a staging area and starts them again (Microkit systems are
         static: there is no restart today)
   - [ ] A/B safety net: a watchdog that rolls back to the last good version if the new one
         does not pass `make test` within N seconds; logs kept across restarts
   - [ ] Native toolchain: clang/LLVM, make, git (needs pthreads and large memory)
   - [ ] Native Claude Code: Node.js (pthreads, libuv, executable mappings for V8's JIT)
14. [ ] **Users and permissions** (M): today everything is root and permissions are not checked;
        real login; process owner (uid/gid) when creating files
15. [ ] **Journal in fs_ext2** (L), towards ext3/ext4
16. [ ] **Booting on real hardware: Dell Latitude 7390 2-in-1, UEFI** (L)
   - [ ] Live ISO: GRUB (multiboot2) loads seL4 + the system + an ext2 disk into RAM
   - [ ] GOP framebuffer (address and format from multiboot2) for the graphical console;
         2x font scaling for HiDPI; scrollback with Shift+PgUp
   - [ ] Persistent disk with the sDDF NVMe driver
   - [ ] Wi-Fi (Intel 8265), USB, audio: passthrough (VT-d) to the Linux VM as a "driver VM"
   - [ ] Touchpad/touchscreen (I²C-HID)
   - [ ] Native networking through the driver VM: virtio-net between netstack and the VM
   - [ ] Alternative to the driver VM for Wi-Fi: the Linux driver inside a component
         (LKL or Genode-style DDE), no VM
   - [ ] Multicore (seL4 runs on a single core today; the laptop has 4)
   - [ ] Release build (kernel and system without debugging)

## Long term

- [ ] io_uring as an interface for programs: not planned (a Linux-only ABI that almost nothing
      uses directly; seL4 PPCs are cheap). The ring model is used between servers instead (item 11)
- [ ] **posix_server is a single point of failure**: if it goes down, all of native goes down;
      split it into servers (processes/memory, files, network, tty)
- [ ] Our own triple (`x86_64-sel4-musl`) instead of `x86_64-linux-musl` (today programs see `__linux__`)
- [ ] Port to ARM64 / Raspberry Pi 4 (sDDF lacks an SD driver)

## Known issues (small, unscheduled)

- [ ] /proc: real loadavg, /proc/<pid>/maps and fd, named tty (today "?")
- [ ] Make `nice` change the real seL4 priority (today the value is only stored)
- [ ] Hard links over 9P (`Tlink` in 9pd; fs_ext2 already supports them)
