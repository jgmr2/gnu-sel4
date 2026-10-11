#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""make test: automated GNU/seL4 tests in headless QEMU.

Copies the system partition of disk.img to a temporary disk (the real disk is never
touched), creates /etc/test on the copy (so /etc/rc runs /etc/tests.sh), boots QEMU and
reads the "TEST ..." lines from the serial port. Exits with 0 only if all of them pass.

  usage: tools/test.py [--no-net] [--timeout SECONDS]
"""
import argparse, fcntl, os, select, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
DISK = os.path.join(PROJ, "disk.img")
BUILD = os.environ.get("BUILD_DIR") or os.path.join(PROJ, "build")   # docker compose: build-docker/
COPY = os.path.join(BUILD, "test-disk.img")
MARKER = os.path.join(BUILD, "test-marker")
PART1_END = (1 + 64) << 20                 # MBR + ext2 partition (EXT2_MB in system.mk)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-net", action="store_true", help="skip the tests that need the internet")
    ap.add_argument("--timeout", type=int, default=240, help="limit in seconds")
    a = ap.parse_args()

    for f in ("sel4_32.elf", "loader.img"):
        if not os.path.exists(os.path.join(BUILD, f)):
            sys.exit("missing build/%s: build with ../../dev.sh make" % f)
    with open(DISK, "r+b") as d:             # do not copy a disk QEMU is using
        try:
            fcntl.lockf(d, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            sys.exit("disk.img is in use (QEMU running?): close it to run the tests")
        fcntl.lockf(d, fcntl.LOCK_UN)
        with open(COPY, "wb") as c:
            c.write(d.read(PART1_END))
            c.truncate(os.path.getsize(DISK))    # the rest (Docker partition) empty, sparse

    with open(MARKER, "w") as m:
        m.write("no-net" if a.no_net else "full")
    # debugfs: directly if it is here (inside the Docker image), otherwise through dev.sh
    top = os.path.dirname(os.path.dirname(PROJ))
    rel = lambda p: os.path.relpath(p, top).replace(os.sep, "/")
    debugfs = ["debugfs"] if shutil.which("debugfs") else [os.path.join(top, "dev.sh"), "debugfs"]
    r = subprocess.run(debugfs + ["-w", "-R", "write %s /etc/test" % rel(MARKER), "%s?offset=1048576" % rel(COPY)],
                       cwd=top, capture_output=True, text=True)
    if r.returncode:
        sys.exit("debugfs: " + r.stderr)

    qemu = ["qemu-system-x86_64", "-machine", "q35", "-accel", "kvm",
            "-cpu", "host,+fsgsbase,+pdpe1gb,+xsaveopt,+xsave,+vmx,+vme",
            "-kernel", os.path.join(BUILD, "sel4_32.elf"), "-initrd", os.path.join(BUILD, "loader.img"),
            "-m", "6G", "-display", "none", "-vga", "none", "-device", "bochs-display,addr=0x5",
            "-serial", "stdio", "-monitor", "none",
            "-drive", "file=%s,format=raw,if=none,id=hd" % COPY, "-device", "virtio-blk-pci,drive=hd,addr=0x3.0",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0,addr=0x4.0"]
    p = subprocess.Popen(qemu, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    os.set_blocking(p.stdout.fileno(), False)
    end, results, buf, t0 = None, [], b"", time.time()
    try:
        while time.time() - t0 < a.timeout and end is None:
            if not select.select([p.stdout], [], [], 1)[0]:
                if p.poll() is not None:
                    break
                continue
            buf += p.stdout.read() or b""
            *lines, buf = buf.split(b"\n")
            for l in lines:
                t = l.decode("utf-8", "replace").rstrip("\r")
                if t.startswith("TEST ") or t.startswith("    "):
                    print(t)
                    if t.startswith("TEST "):
                        results.append(t)
                elif t.startswith("== TESTS: END"):
                    end = t
                elif "netstack: DHCP" in t:
                    print("  [serial] " + t)
                elif "PANIC" in t or "faulting PD" in t:
                    print("  [serial] " + t)
    finally:
        p.kill()
        p.wait()
        os.remove(COPY)
        os.remove(MARKER)

    if end is None:
        print("DID NOT FINISH: the tests did not reach the end in %d s" % a.timeout)
        return 1
    bad = [r for r in results if "FAIL" in r]
    print("%s: %d tests, %d failures (%.0f s)" % ("PASS" if not bad else "FAIL", len(results),
                                                  len(bad), time.time() - t0))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
