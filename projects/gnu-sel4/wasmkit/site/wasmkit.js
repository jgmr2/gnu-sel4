// wasmkit.js: boots a wasmkit system in the page (TODO item 24). What Microkit's tool and loader
// do: the one shared memory, the regions with their contents, the control block of every
// protection domain (wasmkit.h: its channels and name), then one Web Worker per PD (worker.js),
// each told the addresses of its setvars. Then the devices: the terminal (xterm.js) for the
// console PD, and the processes posix_server starts (proc.js), a worker each.
// ?system=<name> picks <name>.json (default: system.json).
// SPDX-License-Identifier: BSD-2-Clause
const CTL = 0x10000, PD_SIZE = 1024, PD0 = CTL + 1024, CH_OFF = 560, NAME_OFF = 688;
const CONSOLE_IN = 0x04220000, CONSOLE_IN_SIZE = 0x1000, CONSOLE_KEYS = 2;
const out = document.getElementById('out');
const log = s => { out.textContent += s; };
const t0 = performance.now();
if (!crossOriginIsolated) log('not cross-origin isolated: no shared memory (serve with COOP/COEP)\n');

const name = new URLSearchParams(location.search).get('system') || 'system';
const sys = await (await fetch(`${name}.json`)).json();
for (const r of sys.regions)
    if (r.file) { r.data = new Uint8Array(await (await fetch(r.file)).arrayBuffer()); r.size = r.data.length; }
const end = Math.max(...sys.regions.map(r => r.addr + r.size));
const memory = new WebAssembly.Memory({ initial: Math.ceil(end / 65536) + 16, maximum: sys.memory_pages, shared: true });
const mem = new Uint8Array(memory.buffer), dv = new DataView(memory.buffer), i32 = new Int32Array(memory.buffer);
for (const r of sys.regions) if (r.data) mem.set(r.data, r.addr);

const idx = Object.fromEntries(sys.pds.map((p, i) => [p.name, i]));
dv.setUint32(CTL, 0x54494b57, true);                 // "WKIT"
dv.setUint32(CTL + 4, sys.pds.length, true);
sys.pds.forEach((p, i) => {
    const base = PD0 + i * PD_SIZE;
    mem.fill(0xff, base + CH_OFF, base + CH_OFF + 128);   // every channel leads nowhere (WK_NONE)
    mem.set(new TextEncoder().encode(p.name.slice(0, 63)), base + NAME_OFF);
});
const link = (from, id, to, their) => { const b = PD0 + idx[from] * PD_SIZE + CH_OFF + id * 2; mem[b] = idx[to]; mem[b + 1] = their; };
for (const c of sys.channels) { link(c.a, c.a_id, c.b, c.b_id); link(c.b, c.b_id, c.a, c.a_id); }

// A notification from the page (a device): the bit in the PD's notes, then its wake
const notify = (pd, ch) => {
    const b = PD0 + idx[pd] * PD_SIZE;
    Atomics.or(new BigInt64Array(memory.buffer, b + 8, 1), 0, 1n << BigInt(ch));
    Atomics.add(i32, b / 4, 1);
    Atomics.notify(i32, b / 4);
};

// The terminal, when there is a console
let term;
if (idx.console !== undefined) {
    out.remove();
    term = new Terminal({ rows: 36, cols: 110, convertEol: false });
    term.open(document.getElementById('terminal'));
    const ring = new Uint32Array(memory.buffer, CONSOLE_IN, 2), cap = CONSOLE_IN_SIZE - 8, enc = new TextEncoder();
    term.onData(s => {
        let tail = Atomics.load(ring, 1);
        for (const b of enc.encode(s)) {
            if (tail - Atomics.load(ring, 0) >= cap) break;          // full: the rest is dropped
            mem[CONSOLE_IN + 8 + tail % cap] = b;
            tail++;
        }
        Atomics.store(ring, 1, tail);
        notify('console', CONSOLE_KEYS);
    });
}
const say = s => term ? term.write(s.replace(/\n/g, '\r\n')) : log(s);

const region = Object.fromEntries(sys.regions.map(r => [r.name, r]));
const procs = [];
say(`wasmkit: ${sys.pds.length} protection domains, ${(end >> 20)} MiB of shared memory, loaded in ${Math.round(performance.now() - t0)} ms\n`);
sys.pds.forEach((p, i) => {
    const w = new Worker('worker.js');
    w.onmessage = ({ data: m }) => {
        if (m.type === 'log') say(m.text);
        else if (m.type === 'term') term.write(m.bytes);
        else if (m.type === 'spawn') {
            procs[m.slot]?.terminate();
            const pw = procs[m.slot] = new Worker('proc.js');
            pw.onerror = e => say(`wasmkit: process in slot ${m.slot}: ${e.message}\n`);
            pw.postMessage({ ...m, sys: memory, server: i, procBufs: region.proc_bufs.addr });
        } else if (m.type === 'end') { procs[m.slot]?.terminate(); procs[m.slot] = undefined; }
    };
    w.onerror = e => say(`wasmkit: ${p.name}: ${e.message}\n`);
    const setvars = sys.setvars.filter(v => v.pd === p.name)
        .map(v => ({ symbol: v.symbol, value: region[v.region].addr + (v.end ? region[v.region].size : 0) }));
    w.postMessage({ memory, wasm: p.wasm, id: i, setvars });
});
