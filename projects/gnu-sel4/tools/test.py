#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""make test: automated GNU/seL4 tests in headless QEMU.

Copies the system partition of disk.img to a temporary disk (the real disk is never
touched), creates /etc/test on the copy (so /etc/rc runs /etc/tests.sh), boots QEMU and
reads the "TEST ..." lines from the serial port. Exits with 0 only if all of them pass.

  usage: tools/test.py [--no-net] [--slow] [--only NAME,...] [--timeout SECONDS]
         tools/test.py --script FILE    runs only FILE (sh) in the guest and prints its output
         tools/test.py --keep-disk ...  leaves the disk copy in build/test-disk.img
                                        (measurements, quick experiments: no test suite)
"""
import argparse, fcntl, os, select, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
DISK = os.path.join(PROJ, "disk.img")
BUILD = os.environ.get("BUILD_DIR") or os.path.join(PROJ, "build")   # docker compose: build-docker/
# The copy goes next to the disk itself (disk.img may be a link to a faster disk)
COPY = os.environ.get("TEST_COPY") or os.path.join(os.path.dirname(os.path.realpath(DISK)), "test-disk.img")   # TEST_COPY=: another
MARKER = os.path.join(BUILD, "test-marker")
SCRIPT = os.path.join(BUILD, "test-script")
CMDS = os.path.join(BUILD, "test-debugfs.cmds")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-net", action="store_true", help="skip the tests that need the internet")
    ap.add_argument("--script", help="run only this shell script in the guest and print its output")
    ap.add_argument("--slow", action="store_true", help="add the expensive tests (bigdisk: ~3 minutes more)")
    ap.add_argument("--only", help="just these tests, comma-separated (e.g. ptytest,jobtest)")
    ap.add_argument("--keep-disk", action="store_true",
                    help="keep the disk copy (build/test-disk.img) afterwards, e.g. to look at what a cut left")
    ap.add_argument("--reuse-copy", action="store_true",
                    help="use the disk copy as it is (e.g. one grown with DISK=... tools/disk-grow.py) and keep it")
    ap.add_argument("--timeout", type=int, default=0, help="limit in seconds (default 240, 480 with --slow)")
    a = ap.parse_args()
    if a.reuse_copy:
        if not os.path.exists(COPY): sys.exit("--reuse-copy: no %s" % COPY)
        a.keep_disk = True
    a.timeout = a.timeout or (480 if a.slow else 240)

    for f in ("sel4_32.elf", "loader.img"):
        if not os.path.exists(os.path.join(BUILD, f)):
            sys.exit("missing build/%s: build with ../../dev.sh make" % f)
    with open(DISK, "r+b") as d:             # do not copy a disk QEMU is using
        try:
            fcntl.lockf(d, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            sys.exit("disk.img is in use (QEMU running?): close it to run the tests")
        fcntl.lockf(d, fcntl.LOCK_UN)
        mbr = d.read(512); d.seek(0)              # MBR + ext2 partition (its size from the MBR)
        part1_end = (int.from_bytes(mbr[454:458], "little") + int.from_bytes(mbr[458:462], "little")) * 512
        if not a.reuse_copy:
            with open(COPY, "wb") as c:
                c.write(d.read(part1_end))
                c.truncate(os.path.getsize(DISK))    # the rest (Docker partition) empty, sparse

    with open(MARKER, "w") as m:
        m.write(" ".join(["no-net" if a.no_net else "full"] + (["slow"] if a.slow else []) +
                         (["only=" + a.only] if a.only else [])))
    # debugfs: directly if it is here (inside the Docker image), otherwise through dev.sh
    top = os.path.dirname(os.path.dirname(PROJ))
    rel = lambda p: os.path.relpath(p, top).replace(os.sep, "/")
    debugfs = ["debugfs"] if shutil.which("debugfs") else [os.path.join(top, "dev.sh"), "debugfs"]
    # The copy is marked "not cleanly unmounted": every run goes through the boot check
    # (read-only mount, e2fsck -p /dev/root in /etc/rc, then read-write; test fsck-boot)
    cmds = ["write %s /etc/test" % rel(MARKER), "ssv state 0"]
    if a.script:
        shutil.copy(a.script, SCRIPT)
        cmds.append("write %s /etc/test.script" % rel(SCRIPT))
    with open(CMDS, "w") as f:
        f.write("\n".join(cmds) + "\n")
    r = subprocess.run(debugfs + ["-w", "-f", rel(CMDS), "%s?offset=1048576" % rel(COPY)],
                       cwd=top, capture_output=True, text=True, errors="replace")
    if r.returncode:
        sys.exit("debugfs: " + r.stderr)

    # As after a power cut: the disk is marked in use and a free count is off; fs_ext2 must
    # notice and recount at mount (the e2fsck at the end checks it did)
    with open(COPY, "r+b") as c:
        sb = (1 << 20) + 1024
        c.seek(sb + 12)                              # s_free_blocks_count
        free = int.from_bytes(c.read(4), "little")
        c.seek(sb + 12); c.write((free + 7).to_bytes(4, "little"))
        c.seek(sb + 58); c.write((0).to_bytes(2, "little"))   # s_state: not clean

    qemu = ["qemu-system-x86_64", "-machine", "q35", "-accel", "kvm",
            # VT-x for the Linux VM, unless the system was built without it (make LINUX_VM=no).
            # 39 physical address bits whatever the host (host-phys-bits=off: -cpu host copies the
            # host's otherwise, and phys-bits alone is ignored): the firmware's window for 64-bit
            # BARs depends on them, and meta.py expects the disk and network BARs at 0x7000000000
            # (with 48, as on GitHub's AMD runners, they go elsewhere: the disk reads as 0 bytes)
            "-cpu", "host,host-phys-bits=off,phys-bits=39,+fsgsbase,+pdpe1gb,+xsaveopt,+xsave%s,+vme,+invtsc"
                    % ("" if os.environ.get("LINUX_VM") == "no" else ",+vmx"),
            "-kernel", os.path.join(BUILD, "sel4_32.elf"), "-initrd", os.path.join(BUILD, "loader.img"),
            "-m", "6G", "-display", "none", "-vga", "none", "-device", "bochs-display,addr=0x5",
            "-serial", "stdio", "-monitor", "none",
            "-drive", "file=%s,format=raw,if=none,id=hd" % COPY, "-device", "virtio-blk-pci,drive=hd,addr=0x3.0",
            "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0,addr=0x4.0"]
    p = subprocess.Popen(qemu, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    os.set_blocking(p.stdout.fileno(), False)
    end, results, buf, t0, done = None, [], b"", time.time(), None   # done: when the end was seen
    serial_log = open(os.path.join(BUILD, "test-serial.log"), "wb")   # everything, for debugging
    try:
        # Follow the serial port live until the guest powers off (the results themselves are
        # read from the disk afterwards: the serial port is shared and can lose a line)
        while time.time() - t0 < a.timeout:
            if not select.select([p.stdout], [], [], 1)[0]:
                if p.poll() is not None:
                    break
                continue
            chunk = p.stdout.read()
            if chunk == b"":                             # EOF: QEMU exited (powered off)
                break
            buf += chunk or b""
            serial_log.write(chunk or b"")
            *lines, buf = buf.split(b"\n")
            for l in lines:
                t = l.decode("utf-8", "replace").rstrip("\r")
                if t.startswith("TEST ") or t.startswith("    "):
                    print(t)
                elif t.startswith("== TESTS: END"):
                    done = time.time()
                elif "[posix]" in t and ("powering off" in t or "halted" in t):
                    print("  [serial] " + t)
                elif "fs_ext2:" in t and "cleanly" in t:
                    print("  [serial] " + t)
                elif "netstack: DHCP" in t:
                    print("  [serial] " + t)
                elif "PANIC" in t or "faulting PD" in t:
                    print("  [serial] " + t)
            if done and time.time() - done > 60:      # it should have powered off by now
                break
        # The tests end with /sbin/poweroff: QEMU must exit by itself, leaving the disk clean
        try:                                             # EOF comes a moment before the exit
            p.wait(timeout=10)
            powered_off = True
        except subprocess.TimeoutExpired:
            powered_off = False
        p.kill()
        p.wait()
        r = subprocess.run(debugfs + ["-R", "cat /var/log/test-results", "%s?offset=1048576" % rel(COPY)],
                           cwd=top, capture_output=True, text=True, errors="replace")
        if a.script:                                     # the script's output, as it is
            print(r.stdout, end="")
        for t in r.stdout.splitlines():
            if t.startswith("TEST "):
                results.append(t)
            elif t.startswith("== TESTS: END"):
                end = t
        if end is not None:
            if not done:                                 # the serial port lost them: show them now
                for t in r.stdout.splitlines():
                    if t.startswith("TEST ") or t.startswith("    "): print(t)
            results.append("TEST poweroff: " + ("ok" if powered_off else "FAIL (QEMU did not exit on its own)"))
            print(results[-1])
            with open(COPY, "rb") as c:
                c.seek((1 << 20) + 1024 + 58)              # partition 1, superblock, s_state
                clean = int.from_bytes(c.read(2), "little") & 1
            results.append("TEST clean-unmount: " + ("ok" if clean else "FAIL (the ext2 state is not clean)"))
            print(results[-1])
            fsck = ["e2fsck"] if shutil.which("e2fsck") else [os.path.join(top, "dev.sh"), "e2fsck"]
            r = subprocess.run(fsck + ["-fn", "%s?offset=1048576" % rel(COPY)], cwd=top, capture_output=True, text=True, errors="replace")
            results.append("TEST e2fsck: " + ("ok" if r.returncode == 0 else "FAIL (exit %d)" % r.returncode))
            print(results[-1])
            if r.returncode:
                for l in (r.stdout + r.stderr).splitlines()[:20]: print("    " + l)
    finally:
        if p.poll() is None:
            p.kill()
        p.wait()
        for f in ((MARKER, SCRIPT, CMDS) if a.keep_disk else (COPY, MARKER, SCRIPT, CMDS)):
            if os.path.exists(f): os.remove(f)

    if end is None:
        print("DID NOT FINISH: the tests did not reach the end in %d s" % a.timeout)
        return 1
    # The guest says how many tests it ran; any missing result counts as a failure
    import re
    m = re.search(r"(\d+) tests\)", end)
    ran = int(m.group(1)) if m else -1
    got = len([r for r in results if r.startswith("TEST ") and not r.split(":")[0][5:] in ("poweroff", "clean-unmount", "e2fsck")])
    if ran != got:
        results.append("TEST results: FAIL (the guest ran %d tests, %d results found)" % (ran, got))
        print(results[-1])
    bad = [r for r in results if "FAIL" in r]
    print("%s: %d tests, %d failures (%.0f s)" % ("PASS" if not bad else "FAIL", len(results),
                                                  len(bad), time.time() - t0))
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
