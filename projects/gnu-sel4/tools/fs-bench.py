#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Disk throughput, GNU/seL4 against Linux under the same QEMU (KVM, 1 CPU, virtio-blk on an image next to
disk.img, ext2 with 4 KiB blocks): tools/fs-bench.sh on both. Linux is the VM's own
kernel (vm/rootfs/out) booted on its own with rdinit=/bin/sh.

  usage: tools/fs-bench.py [linux|sel4|both]
"""
import os, select, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
BUILD = os.environ.get("BUILD_DIR") or os.path.join(PROJ, "build")
BENCH = os.path.join(HERE, "fs-bench.sh")

def linux():
    img = os.path.join(os.path.dirname(os.path.realpath(os.path.join(PROJ, "disk.img"))), "fs-bench-linux.img")
    with open(img, "wb") as f: f.truncate(1200 << 20)
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
    send("cat > /b.sh <<'XEOF'\n" + open(BENCH).read() + "XEOF\nsh /b.sh /mnt; echo END-$((1+1)); poweroff -f\n")
    out, t0 = b"", time.time()
    while time.time() - t0 < 600 and b"END-2" not in out:
        if select.select([q.stdout], [], [], 1)[0]: out += q.stdout.read() or b""
    q.kill(); os.remove(img)
    for l in out.decode(errors="replace").replace("\r", "\n").splitlines():
        if l.startswith("BENCH write") and "MB/s" in l and "$" not in l: print("Linux:    " + l[6:])

def sel4():
    r = subprocess.run([os.path.join(HERE, "test.py"), "--script", BENCH, "--timeout", "600"],
                       capture_output=True, text=True)
    for l in r.stdout.splitlines():
        if l.startswith("BENCH write"): print("GNU/seL4: " + l[6:])

what = sys.argv[1] if len(sys.argv) > 1 else "both"
if what in ("linux", "both"): linux()
if what in ("sel4", "both"): sel4()
