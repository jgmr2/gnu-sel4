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
- **QEMU is the development machine; the real laptop checks it.** Every change is built and
  tested under QEMU; whatever touches the hardware path goes to the laptop as a numbered live ISO
  (`make PLATFORM=latitude7390 iso ventoy`), and what the laptop shows corrects the QEMU numbers.
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
   - [x] The VM image pinned: the base image by digest, every Alpine package (what goes in and
         what builds it) and Docker by SHA-256, in deps.lock (`make vm-lock` moves them on);
         built with no network, the same bytes every time (sorted cpio, fixed dates, gzip -n)
   - [x] A build without the Linux VM (`make LINUX_VM=no`: a kernel without VT-x, no VMM,
         no /dev/linux; 9pd idles): 31 tests pass. GitHub Actions builds and tests it on every
         push (`.github/workflows/test.yml`)
   - [x] The test job builds its own image (with the cache): pulling :latest raced with the
         image workflow. QEMU runs with host-phys-bits=off,phys-bits=39 (with 48 bits the firmware
         puts the 64-bit BARs outside 0x7000000000: reproduced here, and fixed)
   - [ ] On GitHub's AMD runners the virtio disk still reads as 0 bytes (blk_driver asserts),
         even with 39 bits (the e820 map shows they took effect): something else of AMD/KVM there
         (the IOMMU? the PCI config access?). The test step may fail meanwhile (continue-on-error);
         the build must pass. Next: the serial log artefact with sDDF's PCI scan debug output
   - [ ] The full `make test` on a self-hosted runner with VT-x (the workflow's "full" job, by
         hand); a cache for the CI build (from scratch: ~11 minutes)
3. [x] **File system robustness I** (M). Needs: nothing. The disk takes more and more data and
       its only protection is the recount of free blocks
   - [x] e2fsprogs 1.47.4 ported (e2fsck, mke2fs, tune2fs, dumpe2fs, resize2fs in /sbin), on
         /dev/root: the partition as a raw device through fs_ext2's cache (root only; writes only
         while / is read-only). A dirty disk mounts read-only; /etc/rc runs e2fsck -p and then
         `mount -o remount,rw /` (FS_OP_REMOUNT reads the superblock again). make test marks its
         disk dirty, so every run goes through it; e2fsck -fn on the whole disk finds nothing
   - [x] Write ordering: the blocks allocated in an operation are written (and completed) before
         the inodes, bitmaps and blocks that point to them. Measured: QEMU killed in the middle of
         ~600 file writes leaves only lazy free counts and one leaked block, which e2fsck -p
         repairs on its own
4. [~] **Terminals, sessions and job control** (M). Needs: nothing. What makes daily use and ssh real
   - [x] Pseudo-terminals (server/tty.c: the console and 16 ptys, each with its own line
         discipline): `/dev/ptmx`, `/dev/pts/N`, openpty/forkpty, unlockpt, TIOCSWINSZ, ttyname;
         tcdrain/tcflush (TCSBRK, TCFLSH). Test ptytest
   - [x] Sessions and controlling terminals: a session leader takes the terminal it opens (or
         TIOCSCTTY), /dev/tty is the caller's, signal characters go to its foreground group;
         SIGHUP when the master closes or the session leader exits; /proc shows tty_nr and tpgid
   - [x] Job control: SIGSTOP/SIGTSTP/SIGTTIN/SIGTTOU stop (the thread suspended; a process
         stopping itself finishes its call, POSIX_STOPPED parks it), SIGCONT continues, wait4
         WUNTRACED/WCONTINUED, `Ctrl-Z`, SIGTTIN/SIGTTOU (TOSTOP) for background jobs, orphaned
         groups not stopped, state T in /proc. Test jobtest
   - [x] Job control in the shells: zsh and dash, typed at on a pty (test shelltest): Ctrl-Z,
         jobs, bg, fg, Ctrl-C. Each console login is a session of its own with the console as its
         controlling terminal (init, like getty). `Ctrl-C` in the middle of dash's prompt was a bug
         of dash 0.5.13 itself (the same on Linux): patched (ports/dash/ctrl-c-at-prompt.patch)
   - [x] Asynchronous signals to processes that make no system calls (pure loops): the server
         suspends the thread and, outside the runtime (its in_call counter is 0), puts its
         registers on its stack and runs the runtime's entry point, which saves the FPU state,
         delivers the signals and returns to where it was (POSIX_SYS_ASYNC, runtime/crt0.S).
         Test asynctest
   - [x] sudo with use_pty (its default; `!use_pty` gone from new disks): the command runs on a
         pty of its own (test sudo-pty). /etc/profile runs resize only on the console (on a pty it
         ate what was typed ahead). /proc shows the controlling terminal
   - [x] The dropbear ssh server as an s6-rc longrun (sshd: host keys made at first use, in
         /etc/dropbear); the host's port 2223 reaches it (make run, docker compose). Test sshd
   - [~] GNU screen 5.0.2 ported (/bin/screen; getdtablesize() was INT_MAX before item 5's
         limits, and its closeallfiles() polled 2 billion descriptors), but a session does not
         start: `screen -D -m cmd` returns 0 at once without its socket or window. Skipped for
         now; tmux needs libevent
5. [x] **Memory limits** (S). Needs: nothing (fork copies, so counting pages is simple)
   - [x] Resource limits kept and enforced by posix_server (getrlimit, setrlimit, prlimit64;
         inherited by fork): RLIMIT_AS is a process's page budget (a page beyond it: killed,
         "out of memory"), all the processes of a user other than root together at most 3/4 of
         the pool (root keeps the rest to log in and stop them; memfd pages are the shared
         object's, counted once), RLIMIT_NOFILE is the table (256; getdtablesize() was INT_MAX),
         RLIMIT_STACK the 8 MiB stack. Test limittest
   - [x] RLIMIT_NPROC per (real) user, 48 by default, root exempt. More than 64 processes in all:
         item 6
6. [x] **More than 64 processes** (S). Needs: 5. The memory server and dynamic VM memory that were
       here are their own item now (17): costly (several weekends) for little before the real
       hardware
   - [x] Beyond 64 processes: 128 slots (PROC_SLOTS). libmicrokit gave protected() only the
         badge's low 6 bits; posix_server links its own copy of libmicrokit 2.3.1's main loop
         (server/microkit_main.c) with 8, and wait masks are 128 bits. many-procs runs 100
         processes at once; RLIMIT_NPROC's default is 96
7. [ ] **Linux VM integration II** (M–L). Needs: 1 (credentials across the bridge)
   - [ ] Latency: a 9P round trip costs ~4.5 ms under QEMU (several VM exits per message:
         queue kick, interrupt status read, EOI). MSI-X and VIRTIO_F_EVENT_IDX in libvmm
   - [ ] AF_VSOCK for native programs (sockets between native and VM processes)
   - [ ] Direct networking between native and the VM (today around via the host): sDDF vswitch
         or virtio-net between netstack and the VM
   - [ ] Fast shared files: virtio-fs or 9P over shared-memory queues; hard links over 9P
   - [ ] The VM's system on disk instead of an initramfs in RAM (252 MB fixed today)
8. [ ] **Graphical desktop: our own Wayland compositor** (L). Needs: 4 (terminal), 6 (more than
       64 processes), 7 (the VM side). Sway/wlroots dropped: cairo, pango, glib, harfbuzz,
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
9. [ ] **Threads, then the software that needs them** (L). Needs: 6 (memory per thread)
   - [ ] pthreads and `timer_create`: several TCBs in one address space, each with its own IPC
         buffer, system call buffer and endpoint badge; private futexes in the server (musl's
         fast path is atomic in user space). posix_server itself stays single-threaded: it does
         block on fs_ext2 and netstack calls, which is what the rings of item 10 remove
   - [ ] dpkg + apt with our own `.deb` repository built from our ports (Debian's binaries are
         for glibc and Linux); dpkg first (C), apt needs libc++ and threads
10. [ ] **Performance II: rings, asynchronous I/O, zero copy** (L). Needs: 9 (and the real
        hardware's numbers, item 18, before deciding what is worth it there). Measured under QEMU and not worth it yet: what costs today is CPU
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
   - [x] Write-back (October 2026): an operation's dirty blocks stay in fs_ext2's cache and go
         to the disk together (many dirty blocks, sync/fsync, 5 s after the first change, unmount);
         an operation that frees blocks or inodes is written at once, so a freed block is never
         reused before its release is on the disk. Requests up to 1 MiB; the block allocator
         remembers where it was in each group (it rescanned the bitmap bit by bit for every block).
         Measured, 64 MiB copied: 7.2 s -> 1.8-2.2 s, 4124 disk requests -> 142; QEMU killed in the
         middle of writes: only lazy counts to fix, e2fsck -p repairs it
   - [ ] Large writes with their size known up front (José's idea): the region delimited from
         the start (start and length), so the file's blocks are reserved in one go, contiguous,
         and the data goes by reference in a shared region instead of 64 KiB copies, each
         waiting for the disk; with the ordering of item 3, written back in large requests.
         Today's 12 MB/s is the per-operation synchronous write, not locking
11. [ ] **posix_server split and restartable** (L). Needs: 17 (memory already out), 10 (rings
        between components). Today it is a single point of failure for everything native
   - [ ] Separate servers for processes, files, network and the tty, talking over rings
   - [ ] Important state kept outside each one, so a monitor (item 13) can restart it
12. [ ] **Native isolation and process restriction** (M–L). Needs: 11. Ideas from FreeBSD and
        OpenBSD; Docker is not affected (it stays in the VM)
   - [ ] Jails: their own root, processes and network; on seL4 a jail can be a separate
         posix_server instance in its own component (isolation by the kernel). Later OCI
         images on jails
   - [ ] Capsicum (capability mode) or pledge/unveil, enforced where every call already goes
   - [ ] `sysctl` (FreeBSD style) next to the Linux-format /proc
13. [ ] **Self-hosted development loop** (L). Needs: 17 (VM memory), 9 (threads), 11 (restart)
   - [ ] Claude Code + toolchain in the Linux VM, working on the tree through /mnt/sel4
   - [ ] Userland restart: a monitor component that stops the PDs, loads new images and starts
         them again; an A/B watchdog that rolls back if `make test` fails
   - [ ] Native toolchain (clang/LLVM, make, git) and native Claude Code (Node.js)
14. [ ] **File system robustness II: a journal** (L). Needs: 3. Towards ext3/ext4
   - [ ] A block freed and allocated again within one operation is not ordered (none does that
         today); virtio-blk flush (FLUSH requests) for power cuts of the host itself
15. [ ] **Directory services, native: LDAP, Kerberos, Active Directory, Samba** (L). Needs: 9
        (SSSD, winbind and Samba use threads), 1
   - [ ] An LDAP client and a small nscd-protocol daemon (musl asks it for users it does not
         find in /etc/passwd), Kerberos and PAM, joining an AD domain, SMB file sharing
16. [ ] **Toolchain identity and other architectures** (L). Needs: nothing, lowest priority
   - [ ] Our own triple (`x86_64-sel4-musl`) instead of `x86_64-linux-musl` (programs see `__linux__`)
   - [ ] ARM64 / Raspberry Pi 4 (sDDF lacks an SD driver)
17. [ ] **Memory server and dynamic VM memory** (L). Needs: 6. One pool for the native system and
        the VMs instead of 768 MiB fixed for the VM; for a driver VM (item 19) and self-hosting
   - [ ] mem.c moves into its own component that hands pages to posix_server and the VMM (one
         pool, the budgets of item 5); Microkit: a capability to the VM's EPT
   - [ ] Dynamic VM memory: the VM sees plenty of RAM, each page created on first use (EPT fault
         -> page from the pool) and returned when Linux frees it (virtio-balloon, "free page
         reporting"); a cap per VM and a reserve for native
18. [~] **The real laptop, stage 1: the live system works there** (M). Needs: nothing. Dell
       Latitude 7390 2-in-1 (i5-8350U, UEFI). Everything in RAM: the machine's disks are never
       touched (no disk driver at all). Postponed (October 2026): development continues under
       QEMU, and the laptop comes back when the items before this one are done (moved to the end
       of the list, with stages 2 and 3, in October 2026). If the laptop cannot be made to work,
       the fallback is another Intel machine with VT-x and VT-d (an Intel mini PC), not a
       Raspberry Pi: the system is x86 today (libvmm's x86 VMM, the x86 boot and drivers), and ARM
       would be a port first (item 16). Where it stands:
   - [x] `make PLATFORM=latitude7390 iso` and `make ventoy`: a live ISO, numbered by build (in its
         name, the GRUB menu and the log), copied to the Ventoy stick. The file system is an ext2
         image inside fs_ext2; the console on the firmware's 1920x1080 framebuffer (0x60000000);
         the PS/2 keyboard; a network driver with no card; the VM without a disk (Docker in RAM).
         At boot: the suite without network, hardware measurements, the log as QR codes (`qrlog`)
   - [x] Works on the laptop (builds 001-004): UEFI boot through GRUB, seL4 with the IOMMU (VT-d),
         the framebuffer and the keyboard, posix_server, processes and page faults, the RAM file
         system, epoll/eventfd/AF_UNIX/SCM_RIGHTS/memfd (phase1test except its timer part)
   - [x] Bugs only real hardware showed: account files owned by uid 1000 on a fresh disk (fixed);
         the HPET driver losing deadlines already passed when programmed (fixed: a real HPET only
         fires on a match, QEMU fires anyway)
   - [~] **Timers on the laptop: the open problem.** The HPET stops counting when the package
         enters PC10 (Linux disables it on such CPUs since 5.15: "HPET dysfunctional in PC10") and
         uses the local APIC in TSC-deadline mode, which keeps running (the CPU has ARAT and
         tsc_deadline_timer). The TSC is right (1896 MHz measured; the driver now measures it at
         boot and keeps time with it). Build 002: sleep 1 took 2.9 s; 003 (HPET as a 1 ms alarm):
         hung at the timer check; 004 (timer_ticker: the kernel's own timer through MCS budget
         refills, 100 us every 1 ms): 2.4 s once, hung the next time. Nobody has published sDDF or
         libvmm on real x86 (libvmm's manual: QEMU only); sDDF issue #797 (TSC frequency from
         BootInfo) is related and open
   - [ ] Build 005, diagnostics on the screen: HPET interrupts per second, ticker wake-ups per
         second, the measured TSC frequency, deadlines delivered, before and after the timer check
   - [ ] Experiment: keep the CPU out of deep idle states (a lowest-priority component that spins
         instead of letting the core halt): if timing becomes right, PC10 is confirmed; it also
         unblocks the tests meanwhile (at the cost of power and heat)
   - [ ] The real fix, from what 005 shows: the kernel's timer reaching the deadlines reliably
         (the ticker's wake-up latency, the kernel's TSC calibration), or a C-state limit
   - [ ] A smaller ISO (264 MB today: the VM ~105 MB, the ext2 image 96 MB uncompressed): no VM
         in the live system (no network there yet), the ext2 image compressed or sized to its
         contents (~50 MB in all)
   - [ ] Then: the whole battery to the QR codes; revisit the decisions marked "under QEMU"
   - [ ] Power off through \_S5 from the ACPI tables (today: halt, then the power button)
19. [ ] **The real laptop, stage 2: daily development on it** (L). Needs: 18, 17 (memory for the
       driver VM), 7. Realistic in months of steady work; the network is the biggest piece
   - [ ] Persistent disk: the sDDF NVMe driver, only on a partition of its own and only when
         asked for explicitly (Debian's partitions are never touched)
   - [ ] Network: the Wi-Fi card (Intel 8265) passed to a driver VM with VT-d, where Linux's
         iwlwifi and wpa_supplicant already work (credentials outside the repository); its
         traffic is untrusted (netstack only sees frames on a queue); passthrough is new work in
         libvmm. Alternative: the Linux driver inside a component (LKL or Genode-style DDE)
   - [ ] USB: through the driver VM, or natively (sel4-cap's experimental xHCI driver for
         Microkit); a separate USB VM with an input proxy (a compromised VM must not type)
   - [ ] Multicore (seL4 supports it; one core used today; the laptop has 4)
20. [ ] **The real laptop, stage 3: replacing Debian** (L, probably more than a year at this pace).
        Needs: 19
   - [ ] Touchpad and touchscreen (I²C-HID); the internal keyboard and touchpad stay native
   - [ ] Suspend and resume, battery, thermal, brightness (ACPI)
   - [ ] Audio, accelerated graphics (today: the firmware's framebuffer only)
   - [ ] Side channels: kernel mitigations, the VM on its own cores, seL4's domain scheduler
   - [ ] Console scrollback
