// wasmkit.js: boots a wasmkit system in the page (TODO item 24, experiment 1). What Microkit's
// tool and loader do: the one shared memory, the regions with their contents, the control
// block of every protection domain (wasmkit.h: its channels and name), then one Web Worker per
// PD (worker.js), each told the addresses of its setvars.
// SPDX-License-Identifier: BSD-2-Clause
const CTL = 0x10000, PD_SIZE = 1024, PD0 = CTL + 1024, CH_OFF = 560, NAME_OFF = 688;
const out = document.getElementById('out');
const log = s => { out.textContent += s; };
const t0 = performance.now();
if (!crossOriginIsolated) log('not cross-origin isolated: no shared memory (serve with COOP/COEP)\n');

const sys = await (await fetch('system.json')).json();
for (const r of sys.regions)
    if (r.file) { r.data = new Uint8Array(await (await fetch(r.file)).arrayBuffer()); r.size = r.data.length; }
const end = Math.max(...sys.regions.map(r => r.addr + r.size));
const memory = new WebAssembly.Memory({ initial: Math.ceil(end / 65536) + 16, maximum: sys.memory_pages, shared: true });
const mem = new Uint8Array(memory.buffer), dv = new DataView(memory.buffer);
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

const region = Object.fromEntries(sys.regions.map(r => [r.name, r]));
log(`wasmkit: ${sys.pds.length} protection domains, ${(end >> 20)} MiB of shared memory, loaded in ${Math.round(performance.now() - t0)} ms\n`);
sys.pds.forEach((p, i) => {
    const w = new Worker('worker.js');
    w.onmessage = e => log(e.data);
    w.onerror = e => log(`wasmkit: ${p.name}: ${e.message}\n`);
    const setvars = sys.setvars.filter(v => v.pd === p.name)
        .map(v => ({ symbol: v.symbol, value: region[v.region].addr + (v.end ? region[v.region].size : 0) }));
    w.postMessage({ memory, wasm: p.wasm, id: i, setvars });
});
