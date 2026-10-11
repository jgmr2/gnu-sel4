# GNU/seL4 — work plan

Status: `[x]` done · `[~]` in progress · `[ ]` pending.
Cost: **S** = one session or less · **M** = a few sessions · **L** = weeks.

Principles the plan follows:
- **A verified kernel and isolated services**, not "everything verified": posix_server and the
  other components are ordinary C. The goal is that a failure in one service cannot take the
  others with it, so the big ones get split and made restartable, not postponed forever.
- **The Linux VM is not trusted.** It runs Docker and heavy Linux software, but what crosses
  into the native system is small, simple and checked: bytes on the bridge, files through 9P
  with the caller's permissions, pixels and input for its windows.
- **Few layers.** Our own small pieces where the existing ones would bring a large stack along
  (the compositor, the VM bridge); well-known projects where they fit (musl, FreeBSD's TCP/IP,
  s6, GNU).
- **Measure first, then optimise.** `/proc/perf` and `perfstat` exist for that. Numbers taken
  under QEMU (nested virtualisation, the host's page cache as the disk) are marked as such and
  checked again on the real laptop.
- **Cheap and unlocking first; dependencies before what needs them.** Pending items are in the
  order they should be done; each says what it needs.

## Done

1. [x] **Base system and build environment**
   - Microkit SDK 2.3.1 with our patched tool (`make microkit-tool`, patches in
     `projects/gnu-sel4/microkit/`: `<cap_untyped>`, `<cap_cnode>`, `<cap_asid_pool>`,
     `<cap_sched_pool>`, `<cap_endpoint>` in `<cspace>`), builds in Docker (`dev.sh`, LLVM 18),
     QEMU+KVM on the host
   - POSIX layer: musl 1.2.6 ported (system calls -> seL4 IPC), runtime, `sel4-cc`
   - `posix_server`: processes in slots (exec, fork, wait, signals, groups), tty, VFS, pipes,
     `poll`/`select`, clock (RTC + TSC) and timer. The VFS split into modules (vfs.h): pipes and
     FIFOs, eventfd/timerfd, network sockets, AF_UNIX
   - dash 0.5.13.5 (`/bin/sh`); zsh as the login shell; `/sbin/init` + `/etc/rc`
   - Version control on github.com/jgmr2/gnu-sel4; repository cleanup; everything in English
2. [x] **Reproducible builds, licensing and tests**
   - `deps.lock` (every dependency with its SHA-256 or commit) + `fetch-deps.sh` (with patches
     for Microkit and libvmm); a Docker image with all of it and `docker-compose.yml` (build,
     test, shell, vm-rootfs); GitHub Actions publishes the image jgmr2/gnu-sel4-dev
   - BSD-2-Clause for our own code (`LICENSE`, SPDX headers), every project credited in
     `THIRD_PARTY.md`, license texts on the disk in `/usr/share/licenses/`
   - `make test` (`tools/test.py`): a copy of the system partition marked dirty, headless QEMU,
     /etc/tests.sh writes the results to /var/log/test-results (read with debugfs: the serial
     port can lose lines), a 60 s watchdog per test that dumps every process, poweroff; the host
     checks the count, the clean unmount and e2fsck. 33 tests in ~30 s. `NO_NET=1`, `SLOW=1`
     (bigdisk), `tools/test.py --script FILE` (one script, for measurements)
   - `make RELEASE=1`: kernel and system without debugging (~10 % faster on fork/exec/exit
     under QEMU; it loses the debug messages, so debug is the default)
3. [x] **Everyday programs**
   - GNU coreutils 9.12 (101 programs, multi-call, no Linux headers; `uname -o` = GNU/seL4);
     grep 3.12, sed 4.10, findutils 4.11, diffutils 3.12, tar 1.35, gzip 1.15, less 710 (one
     generic script, `ports/gnu/build-gnu.sh`)
   - ncurses 6.6 + terminfo, zsh 5.9.2, vim 9.2 (trimmed runtime, syntax, netrw)
   - procps-ng 4.0.7 (ps, top, free, uptime, pgrep, pkill, pidof, watch, vmstat, pwdx, tload) on
     a virtual `/proc`: per-process CPU from seL4 scheduling contexts, real load average,
     /proc/<pid>/maps and /proc/<pid>/fd; `/proc/perf` + `perfstat COMMAND` (count and TSC cycles
     of every system call, page fault, fork copy, fs_ext2 operation and disk request)
   - `resize` (real terminal size, TIOCSWINSZ + SIGWINCH), `nice`/`renice`
4. [x] **Memory and processes** (`server/mem.c`)
   - Processes on demand from posix_server's untypeds (TCB, address space, CNode, notification),
     pages on fault, returned on exit/exec/munmap; idle memory 924 MiB reserved -> ~4 MiB used.
     Everything in the low 2 GiB (seL4 zeroes a whole untyped block at its first use: host RAM
     4.3 -> 2.2 GB)
   - A direct map of every frame in posix_server (one mapping per page instead of two);
     programs load lazily, 32 KiB of the file per fault (a file about to be written, truncated,
     removed or replaced is first loaded in full into the processes running it). exec
     4.1 -> 0.12 ms, 20 x fork+exec of /bin/true 100 -> 27 ms
   - memfd + mmap MAP_SHARED across processes; file mmap (private copy); flock
   - 64 processes (64-bit wait sets), 256 descriptors per process, 512 open files, 128 pipes,
     128 AF_UNIX sockets, 64 eventfd and 64 timerfd
   - `nice` lowers the seL4 priority (only downwards: seL4 schedules by strict priority, so a
     higher one would stop the others); only root lowers a nice value
   - Measured and left out: copy-on-write fork. fork costs ~1.5 us per page and the mapping call
     is most of it; COW would need a capability copy, a child mapping, a read-only remap of the
     parent and a fault per first write. Revisit with large pages
5. [x] **Files and disk**
   - `fs_ext2`: our own ext2 on sDDF, verified with `e2fsck`; hard links; permission checks
   - Cache: hash table + LRU (16 MiB); the writes of an operation are collected and written at
     its end (sorted, consecutive blocks in one request, all in flight together), so an operation
     is on the disk when it returns; reads gather the missing blocks plus 128 KiB of read-ahead.
     24 MiB from disk 22 -> 260 MB/s read, 1.7 -> 12 MB/s written (under QEMU)
   - 512 MB system partition; `make disk-grow` grows an existing disk keeping everything
   - Clean shutdown: `/sbin/poweroff` (`reboot`, `halt`) brings services and the VM down, the
     file system is unmounted clean and the machine powers off through ACPI (0x604) or restarts
     (0xCF9). Mounted dirty (power cut), the free counts are recounted from the bitmaps. That
     fixes the counts only: deeper damage needs a real fsck (pending item 3)
6. [x] **Networking**
   - `netstack`: the FreeBSD TCP/IP stack from F-Stack 1.25 (`lib/` and `freebsd/`, no DPDK) on
     sDDF networking, with musl inside the PD. TCP, UDP, IPv4/IPv6, ARP, routes, loopback
   - POSIX sockets through posix_server; DHCP client (`net/dhcp.c`), `netcfg` (dhcp/static,
     /etc/netcfg.conf), DNS via /etc/resolv.conf; `/proc/net/dhcp`
   - curl with HTTPS (mbedTLS 3.6 + Mozilla certificates), socat + `nc`, dropbear ssh/scp
     client, `nettest`. Native download ~38 MB/s (the limit is QEMU's user networking)
7. [x] **Linux VM and the bridge**
   - A Linux VM with Docker (libvmm) on the same seL4; its own disk partition and network
   - WSL-style transparent Linux: `docker ...`, `linux command ...`, an interactive pty
   - The bridge over virtio-vsock (vm/virtio_vsock.c, our minimal device in the VMM) instead of
     a virtio console: 4 MiB native -> VM 1.44 s -> 286 ms, VM -> native 2.47 s -> 527 ms
     (under QEMU); libvmm's console patched to copy in chunks
   - `/mnt/sel4` in the VM over 9P (`9pd`), 128 KiB messages; the agent closes the 9P transport
     before unmounting (it used to deadlock when 9pd restarted)
8. [x] **Supervision**
   - Named pipes (`mkfifo`) with POSIX semantics; skalibs 2.15.1, execline 2.9.9, s6 2.15.1,
     s6-rc 0.7.0 (helpers in /usr/libexec)
   - s6-rc: sources in /etc/s6-rc/source (net: oneshot; 9pd: longrun; `default`), compiled at
     every boot; crashed services come back on their own
9. [x] **Users and permissions**
   - uid/gid per process (real, effective, saved), setuid/setgid, supplementary groups, set-ID
     programs, kill permission (cred.c); fs_ext2 checks the search permission on every path;
     the VFS checks open, create, remove, rename, the sticky bit, chmod/chown/utimes, connect
   - `login` (/etc/shadow, SHA-512; a password is chosen on the first login), `passwd`,
     `chpasswd`; root locked, `user` in wheel; sudo 1.9.17p2 (`!use_pty` until there are
     pseudo-terminals). Accounts and network settings are never overwritten by an install
10. [x] **Console and desktop groundwork**
   - `fbcon`: QEMU bochs-display 1280x800, PS/2 keyboard, our own xterm-256color emulator
     (UTF-8, 24-bit colour, alternate screen, DEC graphics), Spleen font; 17x faster than serial
   - What Wayland needs from POSIX (`phase1test`): epoll, eventfd, timerfd, AF_UNIX with
     SCM_RIGHTS and SO_PEERCRED, memfd + shared mmap

## Pending, in order (higher priority and dependencies first)

1. [x] **Security of the boundaries** (S). It was: a normal user was root in the VM and, through
       9P, over the native files (`user` read /etc/shadow). Test `vm-bounds`, usertest checks
   - [x] `/dev/linux` only for root and the wheel group (Docker's group is root-equivalent too)
   - [x] posix_server stamps the caller's effective uid, gid and groups on every BR_OPEN; the
         agent runs the command with them (plus the VM's docker group, HOME in /tmp/home-<uid>);
         only root may ask for the internal @9p-mount
   - [x] 9P per user: the VM mounts with access=user and 9pd serves each attach with that user's
         native credentials (it switches its effective IDs); the VM's root is squashed to nobody,
         so root inside the VM (Docker) has no power over native files
   - [x] reboot(2) and the network configuration ioctl need root; writing /dev/kmsg needs root
2. [ ] **Reliability, reproducibility and CI** (S–M). Needs: 1 (tests for it)
   - [ ] The rare (about 1 boot in 12) SIGABRT of s6rc-oneshot-runner while s6-rc starts it
         (the `net` oneshot does not run; the `s6-rc` test prints /tmp/s6-svscan.log)
   - [ ] The VM image pinned: Alpine packages with exact versions and the base image by digest,
         in deps.lock (today the VM kernel depends on the day it is built)
   - [ ] CI that works: GitHub's runners are VMs themselves, and the full test needs QEMU with
         KVM plus VT-x for the Linux VM inside seL4 (nested twice). A test mode without the Linux
         VM for GitHub Actions on every push, and the full `make test` on a self-hosted runner
3. [ ] **File system robustness I** (M). Needs: nothing. The disk takes more and more data and
       its only protection is the recount of free blocks
   - [ ] e2fsprogs ported: e2fsck run when the disk mounts dirty (and native mke2fs/resize2fs)
   - [ ] Write ordering within an operation where it matters (data before the inode that points
         to it), so a power cut leaves less to repair
4. [ ] **Terminals, sessions and job control** (M). Needs: nothing. What makes daily use and ssh real
   - [ ] Pseudo-terminals: `/dev/ptmx`, `/dev/pts/N`, openpty, grantpt/unlockpt, TIOCSWINSZ
   - [ ] Sessions and controlling terminals: setsid, TIOCSCTTY, tcsetpgrp/tcgetpgrp, SIGHUP;
         terminal signals go to the foreground process group
   - [ ] Job control: `Ctrl-Z` (SIGTSTP), stopped processes, SIGCONT, `fg`/`bg`, SIGTTIN/SIGTTOU;
         `Ctrl-C` in the middle of dash's prompt (today processed with the next line)
   - [ ] Asynchronous signals to processes that make no system calls (pure loops)
   - [ ] With it: the console login as an s6-rc longrun (getty-like); the dropbear ssh server as
         a longrun; sudo without `!use_pty`; /proc shows the tty name; tmux or screen
5. [ ] **Memory limits** (S). Needs: nothing (fork copies, so counting pages is simple)
   - [ ] A page budget per process and per user (like RLIMIT_AS): today one process can take
         the whole 768 MiB pool. Shared (memfd) pages counted once
6. [~] **First boot on the real laptop: Dell Latitude 7390 2-in-1, UEFI** (M). Needs: nothing.
       Early on purpose: the QEMU numbers (VM exits ~1 ms, the disk being the host's cache)
       drove several decisions, and surprises are cheaper now than at the end
   - [x] `make PLATFORM=latitude7390 iso`: a live ISO (GRUB, UEFI or BIOS, e.g. from Ventoy) that
         runs entirely in RAM: the file system is an ext2 image inside fs_ext2 (no disk driver at
         all, the machine's disks are never touched), the console on the firmware's 1920x1080
         framebuffer (0x60000000, read from the running laptop), the PS/2 keyboard, a network
         driver with no card (net/null_driver.c), the VM without a disk (Docker in its RAM)
   - [x] At boot: the test suite without network, hardware measurements (fork/exec, the RAM file
         system, the VM with real VT-x, the bridge, /proc/perf), then the whole log as QR codes
         (`qrlog`, Nayuki's qrcodegen) and the login prompt. Checked in QEMU, also booting the
         ISO itself with OVMF (UEFI): under UEFI seL4 finds no serial port by itself, so a second
         menu entry passes console_port=0x3f8
   - [ ] Boot it on the laptop and bring back the QR codes; revisit the decisions marked
         "under QEMU" with them
   - [ ] Power off through \_S5 from the ACPI tables (today: halt, then the power button)

7. [ ] **Memory server: the first piece out of posix_server** (M–L). Needs: 5 (its budgets)
   - [ ] mem.c moves into its own component that hands pages to posix_server and the VMM (one
         pool, the budgets of item 5); Microkit: a capability to the VM's EPT
   - [ ] Dynamic VM memory: the VM sees plenty of RAM, each page created on first use (EPT fault
         -> page from the pool) and returned when Linux frees it (virtio-balloon, "free page
         reporting"); a cap per VM and a reserve for native
   - [ ] Beyond 64 processes: the PPC badge names 64 slots; a second endpoint or tables
         allocated on demand (a desktop with its services will need more)
8. [ ] **Linux VM integration II** (M–L). Needs: 1 (credentials across the bridge), 7 (memory)
   - [ ] Latency: a 9P round trip costs ~4.5 ms under QEMU (several VM exits per message:
         queue kick, interrupt status read, EOI). MSI-X and VIRTIO_F_EVENT_IDX in libvmm
   - [ ] AF_VSOCK for native programs (sockets between native and VM processes)
   - [ ] Direct networking between native and the VM (today around via the host): sDDF vswitch
         or virtio-net between netstack and the VM
   - [ ] Fast shared files: virtio-fs or 9P over shared-memory queues; hard links over 9P
   - [ ] The VM's system on disk instead of an initramfs in RAM (252 MB fixed today)
9. [ ] **Graphical desktop: our own Wayland compositor** (L). Needs: 4 (terminal), 7 (more than
       64 processes), 8 (the VM side). Sway/wlroots dropped: cairo, pango, glib, harfbuzz,
       freetype, fontconfig, libdrm, libinput would multiply the native attack surface. No
       threads needed: libwayland runs single-threaded
   - [ ] Phase 0b: PS/2 mouse; Latin American keyboard layout; virtual consoles; /dev/input and
         a mappable /dev/fb0 exposed by fbcon
   - [ ] Phase 1b: signalfd, shm_open (/dev/shm), AF_UNIX SEQPACKET, nested epoll in poll
   - [ ] Phase 2: libwayland (server + client; libffi), wayland-protocols (xdg-shell), a
         precompiled XKB keymap sent to the clients
   - [ ] Phase 3: the compositor, a POSIX process in C: libwayland-server, wl_shm, xdg-shell; one
         epoll loop with a 60 Hz timerfd as the frame clock; i3-style tiling and floating windows
         in one tree; CPU rendering of damaged regions (AVX2 if seL4 saves the AVX state: check
         XSAVE); Spleen for decorations; sway-style resize transactions
   - [ ] Phase 4: a native terminal (foot or our own)
   - [ ] Phase 5: VM windows, Qubes/WSLg-style. A headless compositor inside the VM (weston or
         sway headless, Xwayland for X11 programs) speaks the full protocol with the applications;
         only a simple protocol crosses into native (window created, size, title, pixels of the
         damaged region in shared pages, closed; keyboard and mouse back), over vsock. A small
         native proxy checks it and turns each VM window into a normal window with a border colour
         for its origin. The untrusted VM never talks Wayland to the native compositor.
         Clipboard and drag and drop through an explicit channel, not automatically
10. [ ] **Threads, then the software that needs them** (L). Needs: 7 (memory per thread)
   - [ ] pthreads and `timer_create`: several TCBs in one address space, each with its own IPC
         buffer, system call buffer and endpoint badge; private futexes in the server (musl's
         fast path is atomic in user space). posix_server itself stays single-threaded: it does
         block on fs_ext2 and netstack calls, which is what the rings of item 11 remove
   - [ ] dpkg + apt with our own `.deb` repository built from our ports (Debian's binaries are
         for glibc and Linux); dpkg first (C), apt needs libc++ and threads
11. [ ] **Performance II: rings, asynchronous I/O, zero copy** (L). Needs: 6 (measurements on
        real hardware), 10. Measured under QEMU and not worth it yet: what costs today is CPU
   - [x] Asynchronous file reads built, measured and left out: 186 ms against 183 ms synchronous
         next to a cold 24 MiB read (each 64 KiB operation waits ~0.3 ms for the disk)
   - [x] Zero copy measured: a cached 64 KiB read costs 28 us (two copies); mapping cache pages
         would save ~2 ms per 8 MiB
   - [ ] Own ring ABI (submission/completion rings in the shared buffer, the sDDF model); epoll,
         kqueue and poll as thin layers on top; io_uring itself is not planned
   - [ ] A ring between posix_server and fs_ext2 (several operations in flight, sleep until the
         disk notifies) once real disks show the wait
   - [ ] Zero copy where it pays off: shared mmap of files on the cache pages; large read/write
         passing page capabilities
   - [ ] kqueue as the native event API; large pages (2 MiB) for large regions
12. [ ] **posix_server split and restartable** (L). Needs: 7 (memory already out), 11 (rings
        between components). Today it is a single point of failure for everything native
   - [ ] Separate servers for processes, files, network and the tty, talking over rings
   - [ ] Important state kept outside each one, so a monitor (item 14) can restart it
13. [ ] **Native isolation and process restriction** (M–L). Needs: 12. Ideas from FreeBSD and
        OpenBSD; Docker is not affected (it stays in the VM)
   - [ ] Jails: their own root, processes and network; on seL4 a jail can be a separate
         posix_server instance in its own component (isolation by the kernel). Later OCI
         images on jails
   - [ ] Capsicum (capability mode) or pledge/unveil, enforced where every call already goes
   - [ ] `sysctl` (FreeBSD style) next to the Linux-format /proc
14. [ ] **Self-hosted development loop** (L). Needs: 7 (VM memory), 10 (threads), 12 (restart)
   - [ ] Claude Code + toolchain in the Linux VM, working on the tree through /mnt/sel4
   - [ ] Userland restart: a monitor component that stops the PDs, loads new images and starts
         them again; an A/B watchdog that rolls back if `make test` fails
   - [ ] Native toolchain (clang/LLVM, make, git) and native Claude Code (Node.js)
15. [ ] **File system robustness II: a journal** (L). Needs: 3. Towards ext3/ext4
16. [ ] **Full support for the real laptop** (L). Needs: 6, 7 (driver VM memory), 8
   - [ ] Persistent disk with the sDDF NVMe driver; poweroff through \_S5 from the ACPI tables
   - [ ] Wi-Fi (Intel 8265), USB and audio through a driver VM (VT-d): the IOMMU is mandatory,
         its traffic is untrusted, USB in a separate VM with an input proxy; the internal
         keyboard and touchpad (i8042, I²C-HID) stay native
   - [ ] Native networking through the driver VM (virtio-net with netstack); alternative: the
         Linux Wi-Fi driver inside a component (LKL or Genode-style DDE)
   - [ ] Touchpad and touchscreen (I²C-HID)
   - [ ] Side channels: kernel mitigations, the VM on its own cores, seL4's domain scheduler
   - [ ] Multicore (seL4 runs on one core today; the laptop has 4); scrollback in the console
17. [ ] **Directory services, native: LDAP, Kerberos, Active Directory, Samba** (L). Needs: 10
        (SSSD, winbind and Samba use threads), 1
   - [ ] An LDAP client and a small nscd-protocol daemon (musl asks it for users it does not
         find in /etc/passwd), Kerberos and PAM, joining an AD domain, SMB file sharing
18. [ ] **Toolchain identity and other architectures** (L). Needs: nothing, lowest priority
   - [ ] Our own triple (`x86_64-sel4-musl`) instead of `x86_64-linux-musl` (programs see `__linux__`)
   - [ ] ARM64 / Raspberry Pi 4 (sDDF lacks an SD driver)
