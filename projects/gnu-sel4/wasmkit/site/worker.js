// worker.js: one wasmkit protection domain (TODO item 24): its module on the shared memory, its
// setvars written, then wasmkit_run, which never returns (the worker blocks in its waits).
// SPDX-License-Identifier: BSD-2-Clause
onmessage = async ({ data: { memory, wasm, id, setvars } }) => {
    const dec = new TextDecoder();
    const env = {
        memory,
        wk_puts: (p, n) => postMessage(dec.decode(new Uint8Array(memory.buffer, p, n).slice())),
        wk_now_ns: () => BigInt(Math.round((performance.timeOrigin + performance.now()) * 1e6)),
    };
    const { instance } = await WebAssembly.instantiateStreaming(fetch(wasm), { env });
    const dv = new DataView(memory.buffer);
    for (const v of setvars) dv.setUint32(instance.exports[v.symbol].value, v.value, true);
    instance.exports.wasmkit_run(id);
};
