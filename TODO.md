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
- [x] `make test` (`tools/test.py`): copies the system partition, marks it dirty, creates
      /etc/test, boots headless QEMU; /etc/tests.sh runs the tests, writes the results to
      /var/log/test-results (read with debugfs afterwards: the serial port can lose lines) and
      powers off. The host checks the count, the clean unmount and e2fsck. 29 tests in ~48 s.
      `make test NO_NET=1` without internet

## Next, ordered by cost/benefit

Cost: **S** = one session or less · **M** = a few sessions · **L** = weeks. Earlier items are
cheap and unlock a lot; later ones are expensive or only pay off once the earlier ones exist.
Related work is grouped into one item so it is done once. Real hardware stays last on purpose:
first the system has to be worth booting.

Finished (from the previous order):

1. [x] **Everyday tools** (S): GNU grep 3.12, sed 4.10, findutils 4.11 (find, xargs), diffutils
       3.12, tar 1.35, gzip 1.15 and less 710, built by one generic script (`ports/gnu/build-gnu.sh`,
       same recipe as coreutils; gnulib test suites left out). Test `gnu-tools` in `make test`
2. [x] **Licensing** (S): BSD-2-Clause for our own code (`LICENSE`, SPDX headers), every
       project credited in `THIRD_PARTY.md`, license texts on the disk in `/usr/share/licenses/`
3. [x] **Reproducible builds** (S): `deps.lock` (every dependency with its SHA-256 or commit) +
       `fetch-deps.sh`; a Docker image with all of it and the patched Microkit tool, and
       `docker-compose.yml` (build, test, shell, vm-rootfs). A fresh clone builds and passes
       `make test` with Docker alone. Next: CI on GitHub Actions running `make test` on every push
4. [x] **Supervision: `mkfifo` + s6 + s6-rc** (M)
   - [x] Named pipes (`mkfifo`) with POSIX semantics (blocking opens meet, ENXIO, O_RDWR)
   - [x] skalibs 2.15.1, execline 2.9.9 (multicall), s6 2.15.1 and s6-rc 0.7.0 (skarnet, ISC;
         chosen over runit, unmaintained since 2014); helpers in /usr/libexec
   - [x] s6-rc: sources in /etc/s6-rc/source (net: oneshot `netcfg apply`; 9pd: longrun; the
         `default` bundle), compiled at every boot by /etc/rc, s6-svscan on /run/service.
         Crashed services come back on their own: the first step towards restarting the userland.
         Tests `s6` (kill 9pd -> restarted; s6-svc -d/-u) and `s6-rc` (down/up through s6-rc)
   - [x] On the way: AF_UNIX names are socket inodes in ext2 (connect resolves symlinks); chdir
         keeps the physical directory and `..` after a symlink goes to the target's parent
   - Next services as they appear: the console login and sshd (item 13), the compositor (item 15)
5. [x] **Clean shutdown and disk safety** (S–M)
   - [x] `/sbin/poweroff` (`reboot`, `halt`): s6-rc brings the services down, the Linux VM runs
         its shutdown (Docker stopped, its disk synced and unmounted), SIGTERM then SIGKILL to the
         rest, then reboot(2): posix_server unmounts the file system clean, stops every process
         and powers off through ACPI (PM1a_CNT 0x604 on QEMU q35) or restarts through 0xCF9.
         `-f` skips the orderly part. Real hardware: \_S5 from the ACPI tables (item 20)
   - [x] ext2 state flag: in use while mounted, clean after the shutdown; mounted dirty (power cut,
         QEMU closed) -> the free block and inode counts are recounted from the bitmaps (what a
         write-through file system can get wrong), as e2fsck's pass 5
   - [x] `make test` starts from a disk marked dirty with a wrong free count, ends with `poweroff`
         and checks that QEMU exits on its own, that the disk is clean and that e2fsck passes
   - A full fsck and the journal: item 19
6. [x] **Users and permissions** (M)
   - [x] uid/gid per process (real, effective, saved), setuid/setgid, supplementary groups;
         set-ID programs; new files belong to their creator; kill checks permission (cred.c)
   - [x] Permission checks: fs_ext2 checks the search permission of every directory on a path
         (FS_OP_SETCRED tells it who asks); the VFS checks read/write on open, write on the
         directory to create, remove or rename, the sticky bit, owner-or-root for chmod/chown/
         utimes, write to connect to a socket; access() uses the real IDs; installation makes
         everything root-owned with standard modes (/tmp 1777, /root 700, /home/user 700)
   - [x] `login` (/etc/shadow, SHA-512 crypt; a password must be chosen on the first login),
         `passwd` (set-user-ID root) and `chpasswd`; root is locked, `user` is in wheel; init
         runs login on the console. Accounts and network settings are never overwritten
   - [x] sudo 1.9.17p2 (static, no PAM, no plugins), wheel may run anything, its own log;
         `!use_pty` until there are pseudo-terminals (item 13). `su` is not ported: `sudo -s`
   - The login session as a service: item 13; memory limits per user: item 14
   - Later: users from the network: musl asks an nscd-protocol daemon for users it does not
         find in /etc/passwd (or skarnet's nsss): the hook for item 21

Pending:

7. [x] **A bigger system disk** (S): 512 MB ext2 partition (`EXT2_MB`; it was 64 MB and 72 % full).
       `make disk-grow` grows an existing disk keeping everything: it moves the Docker partition
       forward (only the regions with data, the disk stays sparse) and runs resize2fs. The install
       and `make test` read the partition size from the MBR. `make test SLOW=1` adds `bigdisk`
       (writes across block groups and measures the disk speed)
8. [x] **Performance I: measure, then the cheap big wins** (M)
   - [x] Measurements: `/proc/perf` (count and TSC cycles of every system call, page faults,
         fork copies, address-space teardown, calls to fs_ext2 and fs_ext2's own operations,
         cache and disk requests) and `perfstat COMMAND` (what changed while it ran);
         `tools/test.py --script FILE` runs one script in the guest (~20 s, no test suite)
   - [x] Disk throughput: fs_ext2's cache is a hash table + LRU (16 MiB); the writes of an
         operation are collected and written at its end (sorted, consecutive blocks in one
         request, all requests in flight together), still durable when it returns; reads gather
         the missing blocks plus 128 KiB of read-ahead; free counts written at sync/unmount.
         24 MiB read from disk 22 -> 260 MB/s, written 1.7 -> 12 MB/s (the rest is QEMU
         allocating the sparse disk file)
   - [x] Process memory: a direct map of every frame in posix_server (one mapping per page
         instead of two); programs load lazily, 64 KiB of the file at a time when touched (a
         file that is written, truncated, removed or replaced first gets fully loaded into the
         processes running it). exec 4.1 -> 0.12 ms; 20 x fork+exec of /bin/true 100 -> 27 ms
   - [ ] Copy-on-write fork: measured, not worth it on seL4 as things are. fork costs ~1.5 us
         per page and the copy itself is the small part: it is the mapping system call. COW needs
         a capability copy, a child mapping and a read-only remap of the parent per page, plus a
         fault on every first write: as much or more. Revisit with large pages or if fork of big
         processes shows up in the numbers
   - [x] `nice` changes the seL4 priority, only downwards: seL4 schedules by strict priority, so
         nice > 0 runs below normal processes (a busy background job no longer slows down the
         interactive ones: test `nice`) and nice <= 0 stays normal; only root lowers a nice value
   - [x] Lazy loading reads 32 KiB of the program per fault (measured 16/32/64: 16 is ~20 %
         faster warm but twice as slow cold, 64 the reverse)
   - [x] Release build: `make RELEASE=1` (also `run`, `test`), in build-release/. Measured ~10 %
         faster on fork/exec/exit, the same elsewhere; it loses the components' debug messages,
         so debug stays the default
9. [x] **Server structure** (M): the base for rings, threads, isolation and splitting the server
   - [x] `server/vfs.c` split into modules (vfs.h): pipes and FIFOs (vfs_pipe.c), eventfd and
         timerfd (vfs_evfd.c), network sockets and configuration (vfs_net.c), AF_UNIX
         (vfs_unix.c); vfs.c keeps descriptors, paths, permissions, files and the dispatch
   - [x] Wait sets are 64-bit (waitmask_t) and the tables larger: 64 processes (the most the
         6-bit PPC badge can name; beyond that, a second endpoint or the split of item "Long
         term"), 256 descriptors per process, 512 open files, 128 pipes, 128 AF_UNIX sockets,
         64 eventfd and 64 timerfd, 32 VM bridge sessions. Truly dynamic tables (allocated on
         demand from the untyped pool) are left for the posix_server split
   - [x] /proc: real load average (Linux's fixed-point averages, sampled every 5 s),
         /proc/<pid>/maps (runs of present pages: with lazy loading the program shows in
         32 KiB pieces) and /proc/<pid>/fd (Linux names: pipe:[n], socket:[n], anon_inode:...)
10. [~] **Rings and asynchronous I/O** (L). Measured first (2026-10-11), with these findings:
   - [x] Asynchronous file reads were built and measured, then left out (the patch is not in the
         tree): a read that misses the cache returned -EAGAIN, fs_ext2 read the blocks in the
         background (in runs of up to 32) and notified posix_server, which put the process to
         sleep meanwhile. No gain: small cached reads next to a cold 24 MiB read took 186 ms
         against 183 ms synchronous, because each 64 KiB operation waits only ~0.3 ms for the
         disk and posix_server already serves other processes between them; what slows them
         down is CPU, not waiting. Worth it again with slow disks or once posix_server is split
   - [x] Zero copy measured: a cached 64 KiB read costs 28 us in posix_server + fs_ext2
         (~2.3 GB/s, two copies); for `cat` of 8 MiB that is 3.7 of 14 ms (the rest is starting
         the process). Mapping cache pages would save ~2 ms per 8 MiB: later, for large files
   - [ ] Own ring ABI (submission/completion rings in the shared buffer, the sDDF model): many
         operations per notification, nothing blocking; epoll, kqueue and poll become thin layers
         on top. A subset of the io_uring ABI on top of it only if some program needs liburing
   - [ ] Asynchronous I/O in fs_ext2: today it serves one disk request at a time and busy-waits
         for the reply (`blk_io`), so nothing else touching files makes progress. A ring between
         posix_server and fs_ext2: several requests in flight, sleep until the disk notifies
   - [ ] Zero copy where it pays off: shared `mmap` of files mapping the fs_ext2 cache pages
         into the process (today: private copy, or shared read-only); large read/write
         (>= 64 KiB) passing page capabilities instead of copying. Small transfers keep copying:
         mapping + unmapping costs more than copying 4 KiB
   - [ ] kqueue as the native event API (epoll stays for compatibility), on top of the rings
   - [ ] Large pages (2 MiB) for large regions (fork, exec and faults pay per 4 KiB page today);
         free unused page tables
11. [ ] **Linux VM integration** (M–L; the VM runs Linux at native speed with VT-x; what is slow
        is the current bridge: serial-port-style queues with byte-by-byte copies)
   - [ ] vsock (AF_VSOCK): POSIX sockets between native and VM processes (virtio-vsock in
         libvmm, AF_VSOCK in posix_server); `docker`/`linux` over vsock instead of the current
         bridge. virtio rings are zero copy: the biggest copy saving in the whole system
   - [ ] Direct networking between native and the VM (today it goes around via the host):
         sDDF vswitch or virtio-net between netstack and the VM
   - [ ] Fast shared files: 9P or virtio-fs over shared-memory queues; hard links over 9P
         (`Tlink` in 9pd; fs_ext2 already supports them)
   - [ ] Dynamic VM memory: the VM sees plenty of RAM, but each page is created on first use
         (EPT fault in libvmm -> page from the pool) and goes back to the pool when Linux frees
         it (virtio-balloon with "free page reporting"). A cap per VM and a reserve for native
   - [ ] Memory server: move mem.c out of posix_server into a component that hands pages to
         posix_server and the VMM (a single pool, with item 14's budgets); Microkit: capability
         to the VM's EPT
   - [ ] The VM's system on disk instead of an initramfs in RAM (252 MB fixed today)
12. [ ] **Reliability and CI** (S–M): every later change gets checked on its own
   - [x] A watchdog per test in /etc/tests.sh: a test that takes over 60 s fails as hung and
         dumps every process (`ps`), instead of the whole run ending as "did not finish"
   - [x] The VM agent deadlocked when 9pd was restarted after the VM had mounted /mnt/sel4: it
         unmounted (waiting for 9P replies) before closing the transport it forwards itself.
         Only showed up once make test got fast enough for the VM to be up before the `s6` test
   - [ ] The rare (about 1 boot in 12) SIGABRT of s6rc-oneshot-runner while s6-rc starts it (the
         `net` oneshot does not run; the `s6-rc` test prints /tmp/s6-svscan.log when it happens)
   - [ ] GitHub Actions runs `make test NO_NET=1` (KVM on the runner) on every push, with the
         jgmr2/gnu-sel4-dev image; after the flaky failures are gone
13. [ ] **Terminals, sessions and job control** (M): what makes daily use (and ssh) real
   - [ ] Pseudo-terminals: `/dev/ptmx`, `/dev/pts/N`, openpty, grantpt/unlockpt, TIOCSWINSZ
   - [ ] Sessions and controlling terminals: setsid, TIOCSCTTY, tcsetpgrp/tcgetpgrp, SIGHUP when
         the terminal goes away; terminal signals go to the foreground process group
   - [ ] Job control: `Ctrl-Z` (SIGTSTP), stopped processes, SIGCONT, `fg`/`bg`, SIGTTIN/SIGTTOU;
         `Ctrl-C` in the middle of dash's prompt is processed with the next line (today)
   - [ ] Asynchronous signals to processes that make no syscalls (pure loops): suspend the TCB and
         redirect it to the handler
   - [ ] Services that come with it: the console login as an s6-rc longrun (a getty-like
         service, instead of init's loop); the dropbear ssh server as a longrun; sudo without
         `!use_pty`; /proc shows the tty name (today "?"); tmux or screen
14. [ ] **Memory limits** (S–M), on top of item 8's copy-on-write page accounting
   - [ ] Per process and per user (a page budget, like RLIMIT_AS): today one
         process can take the whole 768 MiB pool (a native denial of service). Shared and COW
         pages counted once
15. [ ] **Graphical desktop: our own Wayland compositor** (L), after items 8, 9 and 13. It does
        not need threads: libwayland runs single-threaded (musl's mutexes work without threads).
        Sway/wlroots dropped: cairo, pango, glib, harfbuzz, freetype, fontconfig, libdrm,
        libinput... would multiply the native attack surface. Heavy GUI applications run in the
        Linux VM and reach the native desktop through waypipe (like WSLg)
   - [ ] Phase 0b: PS/2 mouse; Latin American keyboard layout; virtual consoles; an input
         device (/dev/input) and a mappable framebuffer (/dev/fb0) exposed by fbcon
   - [ ] Phase 1b: signalfd (then skalibs can use it too), shm_open (/dev/shm), AF_UNIX
         SEQPACKET, nested epoll in poll
   - [ ] Phase 2: libwayland (server + client; libffi) and wayland-protocols (xdg-shell); a
         precompiled XKB keymap the compositor sends to clients (libxkbcommon only in clients)
   - [ ] Phase 3: the compositor, a POSIX process (not a PD), in C: libwayland-server, wl_shm
         only, xdg-shell; one epoll loop (Wayland sockets, input, a 60 Hz timerfd as the frame
         clock: there is no vsync interrupt); i3-style tiling and floating windows in one tree;
         CPU rendering of damaged regions only (AVX2 alpha blending, if seL4 saves the AVX
         state: check XSAVE in the kernel configuration); bitmap fonts (Spleen) for decorations;
         resizes through xdg-shell configure/ack and sway-style transactions (no flicker)
   - [ ] Phase 4: a native terminal (foot or our own, needs item 13); VM windows through waypipe
         (needs item 11's vsock to be fast); per-origin border colours (native / VM), Qubes-style
16. [ ] **Threads, then software that needs them** (L)
   - [ ] pthreads and `timer_create`: several seL4 TCBs in one address space. Each thread: its
         own TCB, IPC buffer, syscall buffer and endpoint badge; per-process state stays in the
         server. Private futexes in the server (musl's fast path is atomic in user space).
         posix_server stays single-threaded on purpose (it never waits inside a call); scaling
         comes from splitting it into servers on several cores with rings (item 10).
         Blocks browsers, Node.js and almost any large program
   - [ ] Installing software without rebuilding the image: **dpkg + apt** with our own `.deb`
         repository (Debian's binaries are built for glibc and Linux and do not run here: the
         packages are built from our ports, signed, and published e.g. with GitHub Releases or
         Pages). dpkg first (C, possible before threads); apt is C++: libc++ and pthreads.
         Until then, ports organised like FreeBSD's ports tree
17. [ ] **Native isolation and process restriction** (M–L, easier after items 9 and 10), ideas
        from FreeBSD and OpenBSD. Docker is not affected: it keeps running Linux containers in
        the Linux VM
   - [ ] Jails for native programs: their own root, their own visible processes, their own
         network. On seL4 a jail can also be a separate posix_server instance in its own
         component: isolation enforced by the kernel, not by server code. Later, OCI images
         on jails (as FreeBSD's podman + ocijail) for native containers
   - [ ] Process restriction: Capsicum (FreeBSD's capability mode: only the descriptors it
         holds) or pledge/unveil (OpenBSD: "only these kinds of calls, only these paths"),
         enforced in posix_server, through which every call already goes
   - [ ] `sysctl` (FreeBSD style) next to the Linux-format /proc
18. [ ] **Self-hosted development loop** (L): develop GNU/seL4 from inside GNU/seL4,
        autonomously: edit → build → stage → restart the whole userland (not the kernel) →
        read logs and behaviour → commit if it is good
   - [ ] First step: Claude Code + toolchain in the Linux VM, working on the tree through
         `/mnt/sel4` (needs dynamic VM memory, item 11: the build does not fit in 768 MiB)
   - [ ] Userland restart: a monitor component with authority over the other PDs that stops
         them, loads new images from a staging area and starts them again (Microkit systems are
         static: there is no restart today)
   - [ ] A/B safety net: a watchdog that rolls back to the last good version if the new one
         does not pass `make test` within N seconds; logs kept across restarts
   - [ ] Native toolchain: clang/LLVM, make, git (needs threads and large memory)
   - [ ] Native Claude Code: Node.js (threads, libuv, executable mappings for V8's JIT)
19. [ ] **File system robustness** (M–L)
   - [ ] e2fsprogs ported: a full fsck for deeper damage, and native mke2fs/resize2fs
   - [ ] Journal in fs_ext2, towards ext3/ext4 (avoids most fsck runs)
20. [ ] **Booting on real hardware: Dell Latitude 7390 2-in-1, UEFI** (L)
   - [ ] Live ISO: GRUB (multiboot2) loads seL4 + the system + an ext2 disk into RAM
   - [ ] GOP framebuffer (address and format from multiboot2) for the graphical console;
         2x font scaling for HiDPI; scrollback with Shift+PgUp
   - [ ] Persistent disk with the sDDF NVMe driver
   - [ ] Wi-Fi (Intel 8265), USB, audio: passthrough (VT-d) to the Linux VM as a "driver VM",
         Qubes-style: the IOMMU is mandatory (the VM's DMA stays inside its own memory), its
         network traffic is untrusted (netstack only sees frames on a queue), and USB goes to a
         separate VM with an input proxy (a compromised VM must not inject keystrokes); the
         internal keyboard and touchpad (i8042, I²C-HID) stay native
   - [ ] Side channels (Spectre and others): kernel mitigations on; with multicore the VM on
         its own cores; seL4's domain scheduler for strict temporal isolation (time protection
         is research, not in mainline seL4 yet)
   - [ ] Touchpad/touchscreen (I²C-HID)
   - [ ] Native networking through the driver VM: virtio-net between netstack and the VM
   - [ ] Alternative to the driver VM for Wi-Fi: the Linux driver inside a component
         (LKL or Genode-style DDE), no VM
   - [ ] Multicore (seL4 runs on a single core today; the laptop has 4)

21. [ ] **Directory services, native: LDAP, Kerberos, Active Directory, Samba** (L), last on purpose
   - [ ] LDAP client (OpenLDAP's libldap; needs OpenSSL or GnuTLS) and a small nscd-protocol daemon
         backed by LDAP, so `id`, `ls -l`, `ps` see directory users unmodified (item 6's nscd hook)
   - [ ] Kerberos (MIT krb5 or Heimdal) and pluggable authentication (Linux-PAM works with musl)
   - [ ] Joining an Active Directory domain: SSSD or Samba's winbind (both need pthreads)
   - [ ] SMB file sharing: Samba (needs pthreads; waf/Python at build time)

## Long term

- [ ] io_uring as an interface for programs: not planned (a Linux-only ABI that almost nothing
      uses directly; seL4 PPCs are cheap). Our own ring ABI instead (item 10), also between servers
- [ ] **posix_server is a single point of failure**: if it goes down, all of native goes down.
      Split it into servers (processes/memory, files, network, tty) once the rings exist (item 10:
      with rings between components the split costs little), and make each one restartable by
      keeping the important state outside it, so the monitor (item 18) can bring it back
- [ ] Our own triple (`x86_64-sel4-musl`) instead of `x86_64-linux-musl` (today programs see `__linux__`)
- [ ] Port to ARM64 / Raspberry Pi 4 (sDDF lacks an SD driver)

## Known issues (small, unscheduled)

- Moved into the items above: /proc details (9), `nice` (8), hard links over 9P (11),
  the s6rc-oneshot-runner SIGABRT (12)
