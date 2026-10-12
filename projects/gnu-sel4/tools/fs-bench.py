#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Disk throughput, GNU/seL4 against Linux under the same QEMU (KVM, 1 CPU, virtio-blk on an image next to
disk.img, ext2 with 4 KiB blocks): tools/fs-bench.sh on both. Linux is the VM's own
kernel (vm/rootfs/out) booted on its own with rdinit=/bin/sh.

  usage: tools/fs-bench.py [linux|sel4|both] [--gib N]

--gib N: one N GiB file instead (written, synced, read back). GNU/seL4's ext2 partition is far
smaller: grow a disk copy first, which the run then uses (test.py --reuse-copy):
  DISK=<dir of disk.img>/test-disk.img PART=<same dir>/grow-part1.img tools/disk-grow.py MB
"""
import os, select, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
BUILD = os.environ.get("BUILD_DIR") or os.path.join(PROJ, "build")
BENCH = os.path.join(HERE, "fs-bench.sh")
GIB = int(sys.argv[sys.argv.index("--gib") + 1]) if "--gib" in sys.argv else 0
def script():
    return ("BIG=%d\n" % GIB if GIB else "") + open(BENCH).read()

def linux():
    img = os.path.join(os.path.dirname(os.path.realpath(os.path.join(PROJ, "disk.img"))), "fs-bench-linux.img")
    with open(img, "wb") as f: f.truncate(((GIB + 1) << 30) if GIB else (1200 << 20))
    mke2fs = "mke2fs" if subprocess.run(["sh", "-c", "command -v mke2fs"], capture_output=True).returncode == 0 else "/sbin/mke2fs"
    subprocess.run([mke2fs, "-q", "-F", "-t", "ext2", "-b", "4096", img], check=True)
    q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-accel", "kvm", "-cpu", "host", "-smp", "1",
                          "-m", "2G", "-kernel", os.path.join(PROJ, "vm/rootfs/out/vmlinuz"),
                          "-initrd", os.path.join(PROJ, "vm/rootfs/out/initramfs.cpio.gz"),
                          "-append", "console=ttyS0 rdinit=/bin/sh quiet", "-drive", "file=%s,format=raw,if=virtio" % img,
                          "-display", "none", "-serial", "stdio", "-monitor", "none", "-nic", "none"],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    os.set_blocking(q.stdout.fileno(), False)
    send = lambda s: (q.stdin.write(s.encode()), q.stdin.flush())
    time.sleep(4)
    send("mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev; modprobe virtio_blk; "
         "modprobe ext4; mkdir -p /mnt; mount /dev/vda /mnt\n")
    send("cat > /b.sh <<'XEOF'\n" + script() + "XEOF\nsh /b.sh /mnt; echo END-$((1+1)); poweroff -f\n")
    out, t0 = b"", time.time()
    while time.time() - t0 < 1200 and b"END-2" not in out:
        if select.select([q.stdout], [], [], 1)[0]: out += q.stdout.read() or b""
    q.kill(); os.remove(img)
    for l in out.decode(errors="replace").replace("\r", "\n").splitlines():
        if l.startswith("BENCH write") and "MB/s" in l and "$" not in l: print("Linux:    " + l[6:])
        if l.startswith("BENCH-FILE") and "$" not in l: print("Linux:    " + l)

def sel4():
    path = os.path.join(BUILD, "fs-bench-script.sh")
    with open(path, "w") as f: f.write(script())
    r = subprocess.run([os.path.join(HERE, "test.py"), "--script", path, "--timeout", "1200"] +
                       (["--reuse-copy"] if GIB else []), capture_output=True, text=True)
    for l in r.stdout.splitlines():
        if l.startswith("BENCH write"): print("GNU/seL4: " + l[6:])
        if l.startswith("BENCH-FILE") or l.startswith("TEST ") or "FAIL" in l: print("GNU/seL4: " + l)

what = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "both"
if what in ("linux", "both"): linux()
if what in ("sel4", "both"): sel4()
