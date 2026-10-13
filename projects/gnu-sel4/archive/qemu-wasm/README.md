# Archived: the ARM64 build emulated in the browser (qemu-wasm)

Retired on 2026-10-13 in favour of wasmkit (`projects/gnu-sel4/wasmkit`, TODO item 24): GNU/seL4
compiled to WebAssembly instead of emulated there. Kept for reference, not maintained: these
scripts are not built nor tested any more, and the paths in them are the ones they had (they
lived in `tools/wasm/`).

## What it was

GNU/seL4's ARM64 build (`make PLATFORM=qemu-arm`) in ktock's qemu-wasm (QEMU compiled to
WebAssembly with emscripten; its TCG backend turns translated blocks into wasm modules).

- `build-qemu-wasm.sh`: qemu-wasm pinned, aarch64-softmmu, a growable heap.
- `make-snapshot.sh`: a native QEMU built from the same source boots the system once and saves
  the machine at a given prompt (`migrate file:`); the page then starts with `-incoming`.
- `make-site.sh`, `site/`: the page (xterm-pty), the images packed and gzipped, the patches to
  the emscripten JS.
- `patches/qemu-arm-migrate-el2-timers.patch`: QEMU 8.2 migrated only the EL1 generic timers;
  a guest at EL2 (seL4 with hypervisor support) lost its timer across a snapshot. A real QEMU
  fix (newer QEMU may have it).

The last commits where it worked: 266d344 (boots), b907449 (snapshot), 8aa1fc2, 21fb36e,
cad1038 (HTTPS for other devices); `git checkout` one of them to run it again.

## Why it was retired

It works, and it showed GNU/seL4 can run in a browser, but it costs too much for what it gives:
two layers of translation (ARM by TCG, then wasm) made an `ls` take 2.5 s and a cold boot
minutes (the kernel zeroing 1.5 GiB of emulated memory); 3 GB of memory per tab; snapshots to
take again after every change; and zsh crashes qemu-wasm's vCPU thread (a TypeError in its TCI
interpreter calling helper_wfi). wasmkit does the same job at native wasm speed in ~330 MB.

## What it taught (kept in TODO item 22)

- A snapshot (`migrate file:` / `-incoming`) skips everything a cold boot does.
- QEMU on a 32-bit host simulates at most 2047 MiB; seL4's SDK kernel for `qemu_virt_aarch64`
  expects 2 GiB.
- The page must export emscripten's TTY runtime and replace its poll, or QEMU's main loop sleeps
  on the terminal and timers, disk completions and even the VM's start wait for a key.
- sDDF's virtio-blk driver did not negotiate VIRTIO_F_VERSION_1 (fixed for every build:
  `projects/gnu-sel4/libvmm/dep/sddf/`), so QEMU computed the rings' addresses wrong when
  restoring a snapshot.
- The demo disk must be a scrubbed copy (`wasmkit/make-demo-disk.sh`): a used disk carries its
  owner's password hash and shell history.
