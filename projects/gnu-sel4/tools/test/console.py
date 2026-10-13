#!/usr/bin/env python3
# console.py <serial-socket> <step>...: drives a GNU/seL4 serial console for a test, with logs.
# Steps: expect:<text> (until the console prints it; TIMEOUT seconds each), send:<text> (Enter
# added, after SETTLE seconds), absent:<text> (fails if the console ever printed it), exit (until QEMU closes the console: it powered off). Every step logs a JSON line;
# a failed one logs what the console showed last. The end: {"test":...,"ok":...,"ms":...}.
# SPDX-License-Identifier: BSD-2-Clause
import json, os, socket, sys, time
T0 = time.time()
TIMEOUT = float(os.environ.get("TIMEOUT", "40"))
# A pause before typing, as a person would: a program that just printed its prompt may still be
# setting the terminal up (passwd and getpass flush pending input when they turn the echo off)
SETTLE = float(os.environ.get("SETTLE", "0.5"))
NAME = os.environ.get("TEST", "console")
def log(ev, **f): print(json.dumps({"t": round(time.time() - T0, 2), "c": "test", "ev": ev, **f}), flush=True)
s = socket.socket(socket.AF_UNIX)
for _ in range(150):
    try: s.connect(sys.argv[1]); break
    except OSError: time.sleep(0.2)
else:
    log("no-console", path=sys.argv[1]); sys.exit(1)
s.settimeout(0.2)
seen = b""
everything = b""                     # all the console printed: for absent:
ok = True
for step in sys.argv[2:]:
    kind, _, arg = step.partition(":")
    t1 = time.time()
    if kind == "absent":
        bad = arg.encode() in everything + seen
        log("absent", text=arg, ok=not bad)
        if bad: ok = False; break
        continue
    if kind == "send":
        time.sleep(SETTLE)
        s.sendall(arg.encode() + b"\r"); everything += seen; seen = b""
        log("send", text=arg); continue
    closed = False
    while time.time() - t1 < TIMEOUT:
        try:
            d = s.recv(65536)
            if not d: closed = True; break
            seen += d
        except socket.timeout: pass
        if kind == "expect" and arg.encode() in seen: break
    done = closed if kind == "exit" else arg.encode() in seen
    # A failed step: the console's last 1500 bytes, and every line that says FAIL (a test's verdict)
    text = (everything + seen).decode(errors="replace")
    extra = {} if done else {"last": seen[-1500:].decode(errors="replace"),
                             "fail_lines": [l.strip() for l in text.splitlines() if "FAIL" in l][-20:]}
    log(kind, want=arg, ok=done, s=round(time.time() - t1, 2), **extra)
    if not done: ok = False; break
    if kind == "exit": break
print(json.dumps({"test": NAME, "ok": ok, "ms": int((time.time() - T0) * 1000)}))
sys.exit(0 if ok else 1)
