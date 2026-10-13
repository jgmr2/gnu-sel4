// GNU/seL4 (the ARM64 build) in qemu-wasm: QEMU's arguments, as `make run PLATFORM=qemu-arm`.
// SPDX-License-Identifier: BSD-2-Clause
Module['arguments'] = [
    '-M', 'virt,virtualization=on', '-cpu', 'cortex-a57', '-m', '2047M', '-nographic', '-nic', 'none',
    '-accel', 'tcg,tb-size=256',
    '-device', 'loader,file=/pack/loader.img,addr=0x70000000,cpu-num=0',
    '-global', 'virtio-mmio.force-legacy=false',
    '-drive', 'file=/pack/disk.img,format=raw,if=none,id=hd',
    '-device', 'virtio-blk-device,drive=hd,bus=virtio-mmio-bus.1',
];
