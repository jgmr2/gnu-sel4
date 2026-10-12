#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Runs the live system (make PLATFORM=latitude7390 CONSOLE=serial) in QEMU before it goes to real
hardware: no disk, no network card, the console on the serial port. It presses Enter where the
test battery and qrlog ask for it and stops at the login hint. The whole serial output goes to
<build>/smoke.log.

  usage: tools/live-qemu.py [--build DIR] [--timeout SECONDS]
"""
import argparse, os, select, subprocess, time

ap = argparse.ArgumentParser()
ap.add_argument("--build", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                "build-latitude7390"))
ap.add_argument("--timeout", type=int, default=300)
a = ap.parse_args()

q = subprocess.Popen(["qemu-system-x86_64", "-machine", "q35", "-accel", "kvm",
                      "-cpu", "host,phys-bits=39,+fsgsbase,+pdpe1gb,+xsaveopt,+xsave,+vmx,+vme,+invtsc", "-m", "6G",
                      "-kernel", a.build + "/sel4_32.elf", "-initrd", a.build + "/loader.img",
                      "-display", "none", "-vga", "none", "-serial", "stdio", "-monitor", "none", "-nic", "none"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
os.set_blocking(q.stdout.fileno(), False)
log = open(a.build + "/smoke.log", "wb")
buf, t0, enters, done = b"", time.time(), 0, False
while time.time() - t0 < a.timeout and not done:
    if select.select([q.stdout], [], [], 1)[0]:
        d = q.stdout.read() or b""
        if not d:
            break
        log.write(d); log.flush(); buf += d
    if (b"Press Enter" in buf or b"Enter: next" in buf) and enters < 20:
        q.stdin.write(b"\n"); q.stdin.flush(); enters += 1; buf = b""; time.sleep(1)
    done = b"Log in as user" in buf
q.kill(); q.wait()
print("%s after %d s, %d x Enter" % ("finished" if done else "DID NOT FINISH", time.time() - t0, enters))
