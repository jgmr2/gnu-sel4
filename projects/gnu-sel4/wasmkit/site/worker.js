// worker.js: one wasmkit protection domain (TODO item 24): its module on the shared memory, its
// setvars written, then wasmkit_run, which never returns (the worker blocks in its waits).
// What the page gives components (env.*): the console's output, the clock, random numbers, and
// for posix_server the processes (proc.js): their memories are created here, so that copies to
// and from them are immediate; the page starts and ends their workers.
// SPDX-License-Identifier: BSD-2-Clause
onmessage = async ({ data: { memory, wasm, id, setvars } }) => {
    const dec = new TextDecoder(), sysU8 = () => new Uint8Array(memory.buffer);
    const procs = [];                                    // slot -> its WebAssembly.Memory
    const env = {
        memory,
        wk_puts: (p, n) => postMessage({ type: 'log', text: dec.decode(sysU8().slice(p, p + n)) }),
        wk_term_write: (p, n) => postMessage({ type: 'term', bytes: sysU8().slice(p, p + n) }),
        wk_now_ns: () => BigInt(Math.round((performance.timeOrigin + performance.now()) * 1e6)),
        wk_epoch: () => BigInt(Math.floor(Date.now() / 1000)),
        wk_random: () => crypto.getRandomValues(new Uint32Array(1))[0],
        wk_proc_exec(s, mod, len, ino, args, argsLen, argc, envc) {
            const m = new WebAssembly.Memory({ initial: 256, maximum: 16384, shared: true });   // 16 MiB, up to 1 GiB
            procs[s] = m;
            postMessage({ type: 'spawn', slot: s, memory: m, bytes: sysU8().slice(mod, mod + len),
                          args: sysU8().slice(args, args + argsLen), argc, envc });
            return 0;
        },
        wk_proc_end(s) {
            if (!procs[s]) return;
            procs[s] = undefined;
            postMessage({ type: 'end', slot: s });
        },
        wk_proc_write(s, v, src, n) {
            const m = procs[s];
            if (!m || v + n > m.buffer.byteLength) return 0;
            const pm = new Uint8Array(m.buffer);
            if (src) pm.set(sysU8().subarray(src, src + n), v); else pm.fill(0, v, v + n);
            return 1;
        },
        wk_proc_read(s, v, dst, n) {
            const m = procs[s];
            if (!m || v + n > m.buffer.byteLength) return 0;
            sysU8().set(new Uint8Array(m.buffer, v, n), dst);
            return 1;
        },
        wk_proc_size: s => procs[s] ? procs[s].buffer.byteLength : 0,
    };
    const { instance } = await WebAssembly.instantiateStreaming(fetch(wasm), { env });
    const dv = new DataView(memory.buffer);
    for (const v of setvars) dv.setUint32(instance.exports[v.symbol].value, v.value, true);
    instance.exports.wasmkit_run(id);
};
