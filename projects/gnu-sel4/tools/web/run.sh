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
websockify 0.0.0.0:6081 127.0.0.1:4555 >/tmp/websockify-serial.log 2>&1 &
echo "web: open http://localhost"
exec make run DISPLAY_OPTS="-display vnc=127.0.0.1:0 -vga none -device bochs-display,addr=0x5" \
    SERIAL_OPTS="-chardev socket,id=con,host=127.0.0.1,port=4555,server=on,wait=off,telnet=off -serial chardev:con"
