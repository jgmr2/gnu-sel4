// proc.js: one posix_server process on wasmkit (TODO item 24, 1b): its module on a memory of its
// own, and its calls to the server. A call: the process's shared buffer (PROC_BUF_BASE in its
// memory) copied to its megabyte of the system's (proc_bufs, where the server sees it), the
// message registers in its block (wasmkit.h: wk_proc), the server's call lock and call word as
// wasmkit.c's ppcall uses them, then the buffer copied back.
// SPDX-License-Identifier: BSD-2-Clause
const PD0 = 0x10000 + 1024, PD_SIZE = 1024, PROCS = 0x18000, FROM_PROC = 0x100;
const BUF = 0x10000, BUF_SIZE = 0x100000, PATH2 = 0x8000;
const NR = { read: 0, write: 1, pread64: 17, pwrite64: 18, readv: 19, writev: 20, getdents64: 217,
             readlink: 89, readlinkat: 267, getcwd: 79, execve: 59, recvfrom: 45, sendto: 44 };
const RETRY = -0x7000n;

onmessage = async ({ data: { sys, memory, bytes, slot, server, procBufs, args, argc, envc } }) => {
    const sv = PD0 + server * PD_SIZE, me = PROCS + slot * 256, stage = procBufs + slot * BUF_SIZE;
    const i32 = new Int32Array(sys.buffer), mr = new BigInt64Array(sys.buffer, me + 16, 16);
    const sysU8 = new Uint8Array(sys.buffer);
    let wake0 = 0;
    const SEQ = sv / 4, LOCK = sv / 4 + 1, CALL = sv / 4 + 4, FROM = sv / 4 + 5, CH = sv / 4 + 6;
    const INFO = new BigInt64Array(sys.buffer, sv + 32, 2), WAKE = me / 4;

    // how much of the buffer a call reads, and its answer fills (the rest is never looked at)
    const inLen = (n, a3, a4) => (n === NR.write || n === NR.pwrite64 || n === NR.sendto) && !a4 ? Math.min(a3, BUF_SIZE)
        : n === NR.execve || n === NR.writev ? BUF_SIZE : PATH2 + 0x1000;
    const outLen = (n, r) => r > 0 && [NR.read, NR.pread64, NR.readv, NR.getdents64, NR.readlink, NR.readlinkat,
                                       NR.getcwd, NR.recvfrom].includes(n) ? Math.min(r, BUF_SIZE) : 0x1000;

    const env = {
        memory,
        wk_call(n, a1, a2, a3, a4, a5, a6) {
            const N = Number(n), mem = new Uint8Array(memory.buffer);
            sysU8.set(mem.subarray(BUF, BUF + inLen(N, Number(a3), Number(a4))), stage);
            mr[0] = n; mr[1] = a1; mr[2] = a2; mr[3] = a3; mr[4] = a4; mr[5] = a5; mr[6] = a6;
            while (Atomics.compareExchange(i32, LOCK, 0, 1) !== 0) Atomics.wait(i32, LOCK, 1);
            wake0 = Atomics.load(i32, WAKE);
            i32[FROM] = FROM_PROC + slot; i32[CH] = slot;
            INFO[0] = 0n; INFO[1] = 7n;
            Atomics.store(i32, CALL, 1);
            Atomics.add(i32, SEQ, 1); Atomics.notify(i32, SEQ);
            for (let k = 0; Atomics.load(i32, CALL) !== 2; k++) if (k > 20000) Atomics.wait(i32, CALL, 1);
            const r = mr[0];
            Atomics.store(i32, CALL, 0);
            Atomics.store(i32, LOCK, 0); Atomics.notify(i32, LOCK, 1);
            if (r !== RETRY) new Uint8Array(memory.buffer).set(sysU8.subarray(stage, stage + outLen(N, Number(r))), BUF);
            return r;
        },
        wk_wait() { Atomics.wait(i32, WAKE, wake0); },
        wk_mr: i => mr[i],
        wk_now_ns: () => BigInt(Math.round((performance.timeOrigin + performance.now()) * 1e6)),
    };
    const { instance } = await WebAssembly.instantiate(bytes, { env });
    instance.exports._start();
};
