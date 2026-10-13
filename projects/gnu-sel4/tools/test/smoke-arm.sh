#!/bin/sh
# tools/test/smoke-arm.sh [disk.img]: the ARM64 build's quick check after a change, one command:
# boots (no service failure), logs in (a fresh user password), runs the core test programs
# (files, processes, job control, terminals), powers off by PSCI. JSON lines, then one summary
# line {"test":"smoke-arm","ok":...}. Needs `make PLATFORM=qemu-arm image`; the disk is copied
# (default: build-wasm/demo-disk.img, a scrubbed copy of the ARM disk: wasmkit/make-demo-disk.sh).
# SPDX-License-Identifier: BSD-2-Clause
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
DISK=${1:-$HERE/../../build-wasm/demo-disk.img}
P=Smoke-123
TEST=smoke-arm TIMEOUT=${TIMEOUT:-60} exec "$HERE/arm.sh" "$DISK" \
    'expect:login: ' 'absent:s6-rc failed' 'absent:segmentation fault' \
    'send:user' 'expect:New password: ' "send:$P" 'expect:Retype new password: ' "send:$P" 'expect:user@gnu-sel4' \
    'send:r=0; for t in filetest proctest jobtest ptytest; do $t >/tmp/$t.log 2>&1 || { r=1; echo FAILED $t; grep -E "FAIL" /tmp/$t.log | head -5; }; done; echo SMOKE=$r' \
    'expect:SMOKE=0' \
    "send:echo $P | sudo -S poweroff" 'expect:powering off' 'exit'
