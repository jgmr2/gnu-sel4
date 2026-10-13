#!/usr/bin/env python3
# console.py <serial-socket> <step>...: drives a GNU/seL4 serial console for a test, with logs.
# Steps: expect:<text> (until the console prints it; TIMEOUT seconds each), send:<text> (Enter
# added, after SETTLE seconds), exit (until QEMU closes the console: it powered off). Every step logs a JSON line;
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
ok = True
for step in sys.argv[2:]:
    kind, _, arg = step.partition(":")
    t1 = time.time()
    if kind == "send":
        time.sleep(SETTLE)
        s.sendall(arg.encode() + b"\r"); seen = b""
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
    tail = seen[-300:].decode(errors="replace")
    log(kind, want=arg, ok=done, s=round(time.time() - t1, 2), **({} if done else {"last": tail}))
    if not done: ok = False; break
    if kind == "exit": break
print(json.dumps({"test": NAME, "ok": ok, "ms": int((time.time() - T0) * 1000)}))
sys.exit(0 if ok else 1)
