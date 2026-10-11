#!/bin/sh
# /etc/tests.sh: automated tests (make test on the host). /etc/rc runs it only if /etc/test
# exists (make test creates it on a copy of the disk). The results go to /var/log/test-results,
# which make test reads from the disk after the final power off, and to /dev/kmsg (QEMU's
# serial port) to follow them live: the serial port is shared and can lose a line.
#   /etc/test holds words: "no-net" skips the tests that need the internet; "slow" adds the
#   expensive ones (make test SLOW=1: bigdisk, which also measures the disk speed).
#
# SPDX-License-Identifier: BSD-2-Clause
exec >/dev/kmsg 2>&1
RESULTS=/var/log/test-results
: > $RESULTS
say() { echo "$1"; echo "$1" >> $RESULTS; }
# make test with a script (tools/test.py --script FILE): only that script, then power off
if [ -f /etc/test.script ]; then
    sh /etc/test.script >> $RESULTS 2>&1
    say "== TESTS: END (script, 0 tests) =="
    sync
    exec /sbin/poweroff
fi
say "== TESTS: BEGIN =="
mode=" $(cat /etc/test) "
failures=0
total=0

# A test that takes longer than this (seconds) fails as hung, with the state of every process
TEST_TIMEOUT=60

check() {          # check <name> <command...>; output lines starting with "INFO " are always shown
    name=$1; shift
    total=$((total + 1))
    "$@" >/tmp/test.log 2>&1 </dev/null &
    tpid=$!
    ticks=0
    while kill -0 $tpid 2>/dev/null && [ $ticks -lt $((TEST_TIMEOUT * 10)) ]; do sleep 0.1; ticks=$((ticks + 1)); done
    if kill -0 $tpid 2>/dev/null; then
        say "TEST $name: FAIL (hung for $TEST_TIMEOUT s)"
        while IFS= read -r l; do say "    $l"; done </tmp/test.log
        ps -eo pid,ppid,stat,comm,args 2>&1 | while IFS= read -r l; do say "    ps: $l"; done
        pkill -9 -P $tpid 2>/dev/null; kill -9 $tpid 2>/dev/null
        failures=$((failures + 1))
        return
    fi
    if wait $tpid; then
        say "TEST $name: ok"
        while IFS= read -r l; do case $l in "INFO "*) say "    ${l#INFO }";; esac; done </tmp/test.log
    else
        say "TEST $name: FAIL ($?)"
        while IFS= read -r l; do say "    $l"; done </tmp/test.log
        failures=$((failures + 1))
    fi
}

check proctest    proctest
check phase1test  phase1test
check filetest    filetest
check usertest    usertest
check passwd      sh -c 'printf "printf \"n3w-pass\\\\nn3w-pass\\\\n\" | passwd\nexit\n" | login -f user 2>&1 | tail -2
                         grep -q "^user:[$]6[$]" /etc/shadow || { echo "the password was not set"; exit 1; }'
check login       sh -c 'echo "user:s3cret" | chpasswd || { echo "chpasswd failed"; exit 1; }
                         out=$(printf "user\ns3cret\nid -un; id -G; pwd; exit\n" | login 2>&1); echo "$out" | tail -4
                         case "$out" in *user*1000*10*/home/user*) ;; *) echo "login as user failed"; exit 1;; esac
                         grep -q "^user:[$]6[$]" /etc/shadow || { echo "no SHA-512 hash:"; cut -c1-12 /etc/shadow; exit 1; }
                         out=$(printf "user\nwrong\n" | login 2>&1); case "$out" in *"Login incorrect"*) ;; *) echo "$out"; exit 1;; esac'
check sudo        sh -c 'out=$(printf "echo s3cret | sudo -k -S -p \"\" id -u | sed s/^/UID=/\nexit\n" | login -f user 2>&1)
                         case "$out" in *UID=0*) ;; *) echo "$out" | tail -5; exit 1;; esac
                         out=$(printf "echo wrong | sudo -k -S -p \"\" id -u | sed s/^/UID=/\nexit\n" | login -f user 2>&1)
                         case "$out" in *UID=0*) echo "a wrong password worked"; exit 1;; esac
                         grep -q "user.*COMMAND=/bin/id" /var/log/sudo.log || { echo "nothing in /var/log/sudo.log"; exit 1; }'
check lazy-exec   sh -c 'cp /bin/dash /tmp/d1 && cp /bin/dash /tmp/d2 || exit 1
                         /tmp/d1 -c "sleep 1; case x in x) echo alive1;; esac; ulimit -a >/dev/null; exit 3" > /tmp/d1.out & p1=$!
                         /tmp/d2 -c "sleep 1; case x in x) echo alive2;; esac; ulimit -a >/dev/null; exit 4" > /tmp/d2.out & p2=$!
                         sleep 0.3
                         echo garbage > /tmp/garbage; cp /tmp/garbage /tmp/d1
                         rm /tmp/d2; dd if=/dev/zero of=/tmp/fill bs=64k count=64 2>/dev/null
                         wait $p1; r1=$?; wait $p2; r2=$?
                         echo "rewritten: $r1 $(cat /tmp/d1.out); removed: $r2 $(cat /tmp/d2.out)"
                         rm -f /tmp/d1 /tmp/fill /tmp/garbage /tmp/d1.out /tmp/d2.out
                         [ $r1 = 3 ] && [ $r2 = 4 ]'
check nice        sh -c 'loop() { i=0; while [ $i -lt 3000 ]; do i=$((i+1)); done; }
                         t() { a=$(date +%s%N); loop; b=$(date +%s%N); echo $(( (b - a) / 1000000 )); }
                         base=$(t); nice -n 10 sh -c "while kill -0 1 2>/dev/null; do :; done" & hog=$!
                         sleep 0.2; low=$(t); kill $hog; wait $hog 2>/dev/null
                         echo "INFO loop alone $base ms, next to a nice 10 busy process $low ms"
                         [ $low -le $((base * 2 + 5)) ] || exit 1
                         out=$(printf "nice -n -5 true\nexit\n" | login -f user 2>&1)
                         echo "$out" | grep -q "Permission denied" || { echo "user lowered its nice: $out"; exit 1; }'
check many-procs  sh -c 'i=0; while [ $i -lt 50 ]; do sleep 3 & i=$((i+1)); done
                         n=$(ps -e | grep -c " sleep"); echo "INFO 50 background processes started, $n running"
                         wait; [ "$n" -ge 50 ]'
check hardlinks   sh -c 'rm -f /tmp/e1 /tmp/e2; echo x >/tmp/e1 && ln /tmp/e1 /tmp/e2 && [ "$(stat -c %h /tmp/e1)" = 2 ] && rm /tmp/e1 && [ "$(cat /tmp/e2)" = x ] && [ "$(stat -c %h /tmp/e2)" = 1 ] && rm /tmp/e2'
check coreutils   sh -c '[ "$(ls /bin | wc -l)" -gt 100 ] && [ "$(echo hello | tr a-z A-Z)" = HELLO ]'
check gnu-tools   sh -c 'set -e; rm -rf /tmp/gt; mkdir /tmp/gt; cd /tmp/gt; printf "a\nb\nc\n" > f
                         test "$(grep -c b f)" = 1; test "$(sed s/b/B/ f | tr -d "\n")" = aBc
                         test "$(find . -name f)" = ./f; cp f g; echo d >> g; ! diff f g >/dev/null
                         tar czf x.tgz f g; mkdir o; tar xzf x.tgz -C o; cmp f o/f; cmp g o/g
                         test "$(zcat x.tgz | tar tf - | xargs echo)" = "f g"; less -V >/dev/null; cd /; rm -rf /tmp/gt'
check licenses    sh -c 'test -f /usr/share/licenses/gnu-sel4/LICENSE && test -f /usr/share/licenses/coreutils/COPYING && test "$(ls /usr/share/licenses | wc -l)" -ge 25'
check vim         vim -es -c 'qa!'
check vim-netrw   vim -N -es -c 'packadd netrw' -c 'if exists(":Explore") | qa! | else | cquit | endif'
check procps      sh -c 'ps -e | wc -l | test "$(cat)" -ge 3 && free >/dev/null && uptime >/dev/null'
check dhcp        sh -c 'case "$(cat /proc/net/dhcp)" in *bound*10.0.2.16/24*) ;; *) cat /proc/net/dhcp; exit 1;; esac'
check resolv      sh -c 'case "$(cat /etc/resolv.conf)" in *"nameserver 10.0.2.3"*) ;; *) cat /etc/resolv.conf; exit 1;; esac'
check nc          sh -c '(nc -l 7007 >/tmp/nc.out &) ; sleep 1; echo "hello nc" | nc 127.0.0.1 7007; sleep 1; test "$(cat /tmp/nc.out)" = "hello nc"'
check ssh-key     sh -c 'rm -f /tmp/k && dropbearkey -t ed25519 -f /tmp/k >/dev/null && ssh -V 2>&1 | head -1 | test -n "$(cat)"'
check net-static  sh -c 'netcfg static 10.0.2.40/24 gateway 10.0.2.2 dns 10.0.2.3 && case "$(netcfg)" in *static*10.0.2.40/24*) ;; *) netcfg; exit 1;; esac && nettest >/dev/null'
check net-dhcp    sh -c 'netcfg dhcp && sleep 1 && case "$(netcfg)" in *bound*) ;; *) netcfg; exit 1;; esac'
check s6          sh -c 'svstat() { s6-svstat -p /run/service/9pd; }; i=0; while [ "$(svstat)" -le 0 ] 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
                         p1=$(svstat); kill "$p1"; i=0; while { p2=$(svstat); [ "$p2" = "$p1" ] || [ "$p2" -le 0 ]; } && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
                         echo "9pd: $p1 -> $p2"; [ "$p1" -gt 0 ] && [ "$p2" -gt 0 ] && [ "$p1" != "$p2" ] || exit 1
                         s6-svc -wd -T 5000 -d /run/service/9pd && [ "$(svstat)" = -1 ] && s6-svc -wu -T 5000 -u /run/service/9pd && [ "$(svstat)" -gt 0 ]'
check s6-rc       sh -c 'cat /tmp/s6-rc.log; grep -v "^$" /tmp/s6-svscan.log | tail -15; up=$(s6-rc -a list | sort | xargs echo); echo "up: $up"; case " $up " in *" 9pd "*) ;; *) exit 1;; esac
                         case " $up " in *" net "*) ;; *) exit 1;; esac
                         s6-rc -d change 9pd && [ "$(s6-svstat -p /run/service/9pd)" = -1 ] && s6-rc -u change 9pd && [ "$(s6-svstat -p /run/service/9pd)" -gt 0 ]'
check s6-user     sh -c 'out=$(printf "s6-rc -a list | grep -qx 9pd && echo LIST-OK\ns6-rc -d change 9pd 2>/dev/null || echo RC-DENIED\ns6-svc -d /run/service/9pd 2>/dev/null || echo SVC-DENIED\nexit\n" | login -f user 2>&1)
                         echo "$out" | grep -E "OK|DENIED"; [ "$(s6-svstat -p /run/service/9pd)" -gt 0 ] || { echo "user stopped 9pd"; exit 1; }
                         for w in LIST-OK RC-DENIED SVC-DENIED; do echo "$out" | grep -q $w || exit 1; done'
check linux-vm    sh -c 'test "$(linux echo hello from the VM)" = "hello from the VM" && linux sh -c "exit 3"; test $? = 3'
case $mode in *" slow "*)
TEST_TIMEOUT=300
check bigdisk     sh -c 'before=$(df -k / | tail -1 | xargs echo | cut -d" " -f4); echo "free: $before KiB"
                         dd if=/dev/urandom of=/tmp/big.src bs=1M count=96 iflag=fullblock 2>/dev/null
                         t0=$(date +%s); cp /tmp/big.src /tmp/big && sync; t1=$(date +%s); a=$(cksum < /tmp/big); t2=$(date +%s)
                         echo "INFO 96 MiB: write $((t1 - t0)) s, read $((t2 - t1)) s (first group is 128 MiB: this crosses into the next)"
                         n=$(wc -c < /tmp/big); [ "$n" = 100663296 ] || { echo "size $n"; exit 1; }
                         b=$(cksum < /tmp/big.src); rm /tmp/big.src; [ "$a" = "$b" ] || { echo "read back differs: $a / $b"; exit 1; }
                         rm /tmp/big
                         after=$(df -k / | tail -1 | xargs echo | cut -d" " -f4); echo "free after rm: $after KiB"; [ "$after" -ge $((before - 64)) ]'
;; esac
if case $mode in *" no-net "*) true;; *) false;; esac; then
    check nettest-local nettest local
else
    check nettest nettest
    check curl-https sh -c 'curl -sS -o /tmp/ex.html -w "%{http_code}" https://example.com | test "$(cat)" = 200 && test -s /tmp/ex.html'
fi

say "== TESTS: END ($failures failures, $total tests) =="
sync

# Last: an orderly power off (make test checks that QEMU exits on its own and the disk is clean)
exec /sbin/poweroff
