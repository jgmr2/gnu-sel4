#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""make disk-grow: grows the GNU/seL4 ext2 partition of an existing disk.img, keeping everything.

The Docker partition starts right after the ext2 one, so it is moved forward: only the regions
that hold data are copied (the disk is sparse). Then the ext2 file system is checked and grown
with resize2fs. The old disk stays as disk.img.old until you delete it.

  usage: tools/disk-grow.py [MEGABYTES]      (default: EXT2_MB from system.mk)
"""
import fcntl, os, re, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
TOP = os.path.dirname(os.path.dirname(PROJ))
DISK = os.environ.get("DISK") or os.path.join(PROJ, "disk.img")   # DISK=: another disk (tests)
NEW = DISK + ".new"
PART = os.environ.get("PART") or os.path.join(PROJ, "build", "grow-part1.img")   # PART=: elsewhere
SECTOR, MIB = 512, 1 << 20

def partitions(path):
    """[(start, sectors)] of the two MBR primary partitions"""
    with open(path, "rb") as f:
        mbr = f.read(512)
    if mbr[510:512] != b"\x55\xaa":
        sys.exit("%s: no MBR" % path)
    return [(int.from_bytes(mbr[446 + 16 * i + 8:446 + 16 * i + 12], "little"),
             int.from_bytes(mbr[446 + 16 * i + 12:446 + 16 * i + 16], "little")) for i in range(2)]

def tool(name, *args):
    """runs an e2fsprogs tool here if it exists, otherwise inside the development image"""
    rel = [os.path.relpath(a, TOP).replace(os.sep, "/") if os.path.isabs(a) else a for a in args]
    cmd = [name] + list(args) if shutil.which(name) else [os.path.join(TOP, "dev.sh"), name] + rel
    return subprocess.run(cmd, cwd=TOP, capture_output=True, text=True)

def copy_data(src, src_off, dst, dst_off, length):
    """copies length bytes, skipping the holes (SEEK_DATA/SEEK_HOLE): the copy stays sparse"""
    fd = src.fileno()
    pos, end = src_off, src_off + length
    while pos < end:
        try:
            data = os.lseek(fd, pos, os.SEEK_DATA)
        except OSError:                     # no more data
            break
        if data >= end:
            break
        hole = min(os.lseek(fd, data, os.SEEK_HOLE), end)
        src.seek(data)
        dst.seek(dst_off + data - src_off)
        left = hole - data
        while left:
            buf = src.read(min(left, 8 * MIB))
            dst.write(buf)
            left -= len(buf)
        pos = hole

def main():
    if len(sys.argv) > 1:
        mb = int(sys.argv[1])
    else:
        mk = open(os.path.join(PROJ, "system.mk")).read()
        mb = int(re.search(r"^EXT2_MB := (\d+)", mk, re.M).group(1))
    if not os.path.exists(DISK):
        sys.exit("no disk.img: the build creates it with the new size")
    (s1, n1), (s2, n2) = partitions(DISK)
    if n1 * SECTOR >= mb * MIB:
        print("the ext2 partition already has %d MiB" % (n1 * SECTOR // MIB))
        return 0
    if os.path.exists(DISK + ".old"):
        sys.exit("disk.img.old exists (a previous grow): delete it first")
    with open(DISK, "r+b") as d:            # never while QEMU uses it
        try:
            fcntl.lockf(d, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            sys.exit("disk.img is in use (QEMU running?): close it first")
        fcntl.lockf(d, fcntl.LOCK_UN)

    os.makedirs(os.path.dirname(PART), exist_ok=True)
    print("ext2: %d -> %d MiB; the Docker partition (%d MiB) moves forward" %
          (n1 * SECTOR // MIB, mb, n2 * SECTOR // MIB))
    # 1. the ext2 partition alone, checked and grown
    with open(DISK, "rb") as d, open(PART, "wb") as p:
        copy_data(d, s1 * SECTOR, p, 0, n1 * SECTOR)
        p.truncate(n1 * SECTOR)
    r = tool("e2fsck", "-fy", PART)
    if r.returncode not in (0, 1):          # 1: errors corrected
        sys.exit("e2fsck (exit %d):\n%s%s" % (r.returncode, r.stdout, r.stderr))
    r = tool("resize2fs", PART, "%dM" % mb)
    if r.returncode:
        sys.exit("resize2fs:\n" + r.stdout + r.stderr)

    # 2. the new disk: same MBR with the new sizes, ext2 grown, Docker partition moved
    ns2 = s1 + mb * MIB // SECTOR
    with open(DISK, "rb") as d, open(NEW, "wb") as n, open(PART, "rb") as p:
        n.truncate((ns2 + n2) * SECTOR)
        mbr = bytearray(d.read(s1 * SECTOR))         # MBR and the gap up to partition 1
        mbr[446 + 12:446 + 16] = (mb * MIB // SECTOR).to_bytes(4, "little")
        mbr[446 + 16 + 8:446 + 16 + 12] = ns2.to_bytes(4, "little")
        n.seek(0); n.write(mbr)
        copy_data(p, 0, n, s1 * SECTOR, mb * MIB)
        copy_data(d, s2 * SECTOR, n, ns2 * SECTOR, n2 * SECTOR)
    os.remove(PART)
    r = tool("e2fsck", "-fn", NEW + "?offset=%d" % (s1 * SECTOR))
    if r.returncode:
        os.remove(NEW)
        sys.exit("the grown file system does not pass e2fsck: disk.img left as it was\n" + r.stdout)
    os.rename(DISK, DISK + ".old")
    os.rename(NEW, DISK)
    print("done: disk.img has ext2 %d MiB; the old disk is disk.img.old (delete it when the new "
          "one boots)" % mb)
    return 0

if __name__ == "__main__":
    sys.exit(main())
