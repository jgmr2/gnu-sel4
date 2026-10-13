#!/usr/bin/env python3
# console-hub.py <qemu-port> <listen-port>: the serial console for many viewers at once. QEMU's
# socket chardev serves one client: this keeps that one connection and fans it out. Every
# client gets what the console prints (and, when it connects, the last 64 KiB of it, so a page
# opened later is not blank); what any of them types goes to the console. websockify serves the
# clients to the page (tools/web/run.sh).
# SPDX-License-Identifier: BSD-2-Clause
import asyncio, sys

QEMU_PORT, LISTEN_PORT = int(sys.argv[1]), int(sys.argv[2])
HISTORY = 64 * 1024
clients, history = set(), bytearray()
qemu_writer = None


async def from_qemu():
    global qemu_writer
    while True:
        try:
            reader, qemu_writer = await asyncio.open_connection('127.0.0.1', QEMU_PORT)
        except OSError:
            await asyncio.sleep(0.5)                 # QEMU not listening yet
            continue
        while data := await reader.read(65536):
            history.extend(data)
            del history[:-HISTORY]
            for w in list(clients):
                try:
                    w.write(data)
                except Exception:
                    clients.discard(w)
        qemu_writer = None                           # QEMU went away: connect again
        await asyncio.sleep(0.5)


async def client(reader, writer):
    writer.write(bytes(history))
    clients.add(writer)
    try:
        while data := await reader.read(4096):
            if qemu_writer:
                qemu_writer.write(data)
    finally:
        clients.discard(writer)
        writer.close()


async def main():
    server = await asyncio.start_server(client, '127.0.0.1', LISTEN_PORT)
    await asyncio.gather(from_qemu(), server.serve_forever())

asyncio.run(main())
