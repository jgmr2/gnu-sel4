#!/bin/sh
# tools/web/run.sh: GNU/seL4 in QEMU with its screen and serial console in the browser.
# Started by `./web.sh` (docker compose up web); then open http://localhost.
#   6080 (the host's port 80): the page, and the screen through noVNC (websockify -> QEMU's VNC)
#   6081: the serial console, a WebSocket to QEMU's serial socket (the page's terminal)
# After rebuilding (../../dev.sh make), reload with `docker compose restart web`.
#
# SPDX-License-Identifier: BSD-2-Clause
set -eu
cd "$(dirname "$0")/../.."
rm -rf /tmp/www && mkdir -p /tmp/www
cp -r /usr/share/novnc /tmp/www/novnc
cp tools/web/index.html /tmp/www/
x=$(ls -d ../../third_party/xterm-* | head -1)
cp "$x/lib/xterm.js" "$x/css/xterm.css" /tmp/www/
websockify --web /tmp/www 0.0.0.0:6080 127.0.0.1:5900 >/tmp/websockify-vnc.log 2>&1 &
# The serial console: QEMU's socket (one client) fanned out to every viewer (console-hub.py)
python3 tools/web/console-hub.py 4555 4560 >/tmp/console-hub.log 2>&1 &
websockify 0.0.0.0:6081 127.0.0.1:4560 >/tmp/websockify-serial.log 2>&1 &
echo "web: open http://localhost"
SOCK="-chardev socket,id=con,host=127.0.0.1,port=4555,server=on,wait=off,telnet=off"
if [ "${PLATFORM:-}" = qemu-arm ]; then
    # ARM has no screen of its own yet: its serial console goes both to the page's terminal and
    # to a QEMU text console, which is the VNC screen (QEMU's chardev hub: either one types)
    exec make run ARM_DISPLAY_OPTS="-display vnc=127.0.0.1:0" \
        SERIAL_OPTS="$SOCK -chardev vc,id=vc,cols=120,rows=40 -chardev hub,id=hub,chardevs.0=con,chardevs.1=vc -serial chardev:hub"
fi
exec make run DISPLAY_OPTS="-display vnc=127.0.0.1:0 -vga none -device bochs-display,addr=0x5" \
    SERIAL_OPTS="$SOCK -serial chardev:con"
