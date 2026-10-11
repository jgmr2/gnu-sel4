#!/bin/sh
# /etc/tests.sh: automated tests (make test on the host). /etc/rc runs it only if /etc/test
# exists (make test creates it on a copy of the disk). Everything goes to /dev/kmsg, which
# reaches QEMU's serial port; make test reads it from there.
#   /etc/test containing "no-net": skip the tests that need the internet.
#
# SPDX-License-Identifier: BSD-2-Clause
exec >/dev/kmsg 2>&1
echo "== TESTS: BEGIN =="
failures=0

check() {          # check <name> <command...>
    name=$1; shift
    if "$@" >/tmp/test.log 2>&1; then
        echo "TEST $name: ok"
    else
        echo "TEST $name: FAIL ($?)"
        while IFS= read -r l; do echo "    $l"; done </tmp/test.log
        failures=$((failures + 1))
    fi
}

check proctest    proctest
check phase1test  phase1test
check filetest    filetest
check hardlinks   sh -c 'rm -f /tmp/e1 /tmp/e2; echo x >/tmp/e1 && ln /tmp/e1 /tmp/e2 && [ "$(stat -c %h /tmp/e1)" = 2 ] && rm /tmp/e1 && [ "$(cat /tmp/e2)" = x ] && [ "$(stat -c %h /tmp/e2)" = 1 ] && rm /tmp/e2'
check coreutils   sh -c '[ "$(ls /bin | wc -l)" -gt 100 ] && [ "$(echo hello | tr a-z A-Z)" = HELLO ]'
check gnu-tools   sh -c 'set -e; rm -rf /tmp/gt; mkdir /tmp/gt; cd /tmp/gt; printf "a\nb\nc\n" > f
                         test "$(grep -c b f)" = 1; test "$(sed s/b/B/ f | tr -d "\n")" = aBc
                         test "$(find . -name f)" = ./f; cp f g; echo d >> g; ! diff f g >/dev/null
                         tar czf x.tgz f g; mkdir o; tar xzf x.tgz -C o; cmp f o/f; cmp g o/g
                         test "$(zcat x.tgz | tar tf - | xargs echo)" = "f g"; less -V >/dev/null; cd /; rm -rf /tmp/gt'
check licenses    sh -c 'test -f /usr/share/licenses/gnu-sel4/LICENSE && test -f /usr/share/licenses/coreutils/COPYING && test "$(ls /usr/share/licenses | wc -l)" -ge 25'
check vim         vim -es -c 'qa!'
check procps      sh -c 'ps -e | wc -l | test "$(cat)" -ge 3 && free >/dev/null && uptime >/dev/null'
check dhcp        sh -c 'case "$(cat /proc/net/dhcp)" in *bound*10.0.2.16/24*) ;; *) cat /proc/net/dhcp; exit 1;; esac'
check resolv      sh -c 'case "$(cat /etc/resolv.conf)" in *"nameserver 10.0.2.3"*) ;; *) cat /etc/resolv.conf; exit 1;; esac'
check nc          sh -c '(nc -l 7007 >/tmp/nc.out &) ; sleep 1; echo "hello nc" | nc 127.0.0.1 7007; sleep 1; test "$(cat /tmp/nc.out)" = "hello nc"'
check ssh-key     sh -c 'rm -f /tmp/k && dropbearkey -t ed25519 -f /tmp/k >/dev/null && ssh -V 2>&1 | head -1 | test -n "$(cat)"'
check net-static  sh -c 'netcfg static 10.0.2.40/24 gateway 10.0.2.2 dns 10.0.2.3 && case "$(netcfg)" in *static*10.0.2.40/24*) ;; *) netcfg; exit 1;; esac && nettest >/dev/null'
check net-dhcp    sh -c 'netcfg dhcp && sleep 1 && case "$(netcfg)" in *bound*) ;; *) netcfg; exit 1;; esac'
check linux-vm    sh -c 'test "$(linux echo hello from the VM)" = "hello from the VM" && linux sh -c "exit 3"; test $? = 3'
if [ "$(cat /etc/test)" = no-net ]; then
    check nettest-local nettest local
else
    check nettest nettest
    check curl-https sh -c 'curl -sS -o /tmp/ex.html -w "%{http_code}" https://example.com | test "$(cat)" = 200 && test -s /tmp/ex.html'
fi

echo "== TESTS: END ($failures failures) =="
