#!/bin/sh
# /etc/tests.sh: automated tests (make test on the host). /etc/rc runs it only if /etc/test
# exists (make test creates it on a copy of the disk). The results go to /var/log/test-results,
# which make test reads from the disk after the final power off, and to /dev/kmsg (QEMU's
# serial port) to follow them live: the serial port is shared and can lose a line.
#   /etc/test holds words: "no-net" skips the tests that need the internet; "slow" adds the
#   expensive ones (make test SLOW=1: bigdisk, which also measures the disk speed); "live" is
#   the live system on real hardware (make PLATFORM=... iso): everything on the screen, hardware
#   measurements after the tests, the results as QR codes (qrlog), and then the login prompt
#   instead of powering off. "only=a,b": just those tests (make test ONLY=a,b), for quick runs
#   while working on one thing.
#
# SPDX-License-Identifier: BSD-2-Clause
case " $(cat /etc/test 2>/dev/null) " in
*" live "*) LIVE=1; exec >/dev/console 2>&1; printf '\033[2J\033[H' ;;
*)          LIVE=; exec >/dev/kmsg 2>&1 ;;
esac
RESULTS=/var/log/test-results
: > $RESULTS
say() {
    if [ -n "$LIVE" ]; then printf '\r\033[K%s\n' "$1"; else echo "$1"; fi
    echo "$1" >> $RESULTS
}
# make test with a script (tools/test.py --script FILE): only that script, then power off
if [ -f /etc/test.script ]; then
    sh /etc/test.script >> $RESULTS 2>&1
    say "== TESTS: END (script, 0 tests) =="
    sync
    exec /sbin/poweroff
fi
say "== TESTS: BEGIN =="
if [ -n "$LIVE" ]; then
    say "GNU/seL4 live build $(cat /etc/live-build 2>/dev/null || echo '?')"
    # Everything below waits on timers: if this line stays, timer interrupts are not arriving
    printf '%s' "Checking the timer (sleep 1 s; if this line stays here, timer interrupts do not arrive)... "
    t0=$(date +%s%N); sleep 1; t1=$(date +%s%N)
    say "timer ok: sleep 1 took $(( (t1 - t0) / 1000000 )) ms"
    say "(each test shows 'running...' while it runs; a hung one fails after 60 s)"
fi
mode=" $(cat /etc/test) "
failures=0
total=0

# A test that takes longer than this (seconds) fails as hung, with the state of every process
TEST_TIMEOUT=60

only=
for w in $mode; do case $w in only=*) only=",${w#only=},";; esac; done

check() {          # check <name> <command...>; output lines starting with "INFO " are always shown
    name=$1; shift
    case "$only" in ""|*",$name,"*) ;; *) return 0;; esac
    total=$((total + 1))
    [ -n "$LIVE" ] && printf '  [%2d] %-14s running...\r' "$total" "$name"
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
check ptytest     ptytest
check jobtest     jobtest
check asynctest   asynctest
check shelltest   shelltest
check limittest   limittest
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
# sudo runs the command on a pseudo-terminal of its own (use_pty, sudo's default; disks made
# before the ptys still have "Defaults !use_pty": taken out here, on the test's copy). socat -t:
# after the input ends it waits for the session (a new home's first zsh is slow: compinit)
check sudo-pty    sh -c 'cp /etc/sudoers /tmp/sudoers.orig && sed "/use_pty/d" /tmp/sudoers.orig > /etc/sudoers
                         out=$(printf "readlink /proc/self/fd/1\necho s3cret | sudo -k -S -p \"\" readlink /proc/self/fd/1\nexit\n" |
                               timeout 30 socat -t 20 - EXEC:"login -f user",pty,setsid,ctty,stderr 2>&1)
                         cat /tmp/sudoers.orig > /etc/sudoers
                         n=$(echo "$out" | grep -o "^/dev/pts/[0-9]*" | sort -u | wc -l)
                         [ "$n" = 2 ] || { echo "expected the login pty and another one for sudo:"; echo "$out" | tail -8; exit 1; }'
# GNU screen: a detached session runs a shell on a pty of its own (packet mode, TIOCPKT); commands
# reach it through its socket (-X stuff), what its window shows comes back (hardcopy), quit ends it
check screen      sh -c 'export SCREENDIR=/tmp/screens; rm -rf $SCREENDIR /tmp/scr.*; mkdir -p -m 700 $SCREENDIR
                         screen -d -m -S t sh || { echo "screen -d -m: exit $?"; exit 1; }
                         i=0; until screen -ls | grep -q "[.]t"; do sleep 0.1; i=$((i+1)); [ $i -le 50 ] || { echo "no session"; exit 1; }; done
                         screen -S t -X stuff "echo screen-ok-\$((6*7)) > /tmp/scr.out; tty > /tmp/scr.tty
"
                         i=0; until [ -s /tmp/scr.tty ]; do sleep 0.1; i=$((i+1)); [ $i -le 50 ] || { echo "the command did not run"; exit 1; }; done
                         grep -q screen-ok-42 /tmp/scr.out || { echo "wrong output: $(cat /tmp/scr.out)"; exit 1; }
                         grep -q "^/dev/pts/" /tmp/scr.tty || { echo "not on a pty: $(cat /tmp/scr.tty)"; exit 1; }
                         screen -S t -X hardcopy /tmp/scr.hc
                         i=0; until grep -q screen-ok /tmp/scr.hc 2>/dev/null; do sleep 0.1; i=$((i+1)); [ $i -le 50 ] || { echo "hardcopy: $(head -3 /tmp/scr.hc 2>&1)"; exit 1; }; done
                         screen -S t -X quit
                         i=0; while screen -ls | grep -q "[.]t"; do sleep 0.1; i=$((i+1)); [ $i -le 50 ] || { echo "still there after quit"; exit 1; }; done'
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
# 100: more than the 64 the PPC badge used to allow (PROC_SLOTS is 128; the services take some)
check many-procs  sh -c 'i=0; while [ $i -lt 100 ]; do sleep 4 & i=$((i+1)); done
                         k=0; while n=$(ps -e | grep -c " sleep"); [ "$n" -lt 100 ] && [ $k -lt 30 ]; do sleep 0.1; k=$((k+1)); done
                         echo "INFO 100 background processes started, $n running"
                         wait; [ "$n" -ge 100 ]'
check proc-files  sh -c 'grep -q "\[stack\]" /proc/self/maps || { echo "no stack in maps"; exit 1; }
                         sh -c "exec 3</etc/passwd; readlink /proc/\$\$/fd/3" | grep -qx /etc/passwd || { echo "fd/3 is not /etc/passwd"; exit 1; }
                         set -- $(cat /proc/loadavg); case $1 in *.*) ;; *) exit 1;; esac'
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
[ -n "$LIVE" ] ||   # live on real hardware: no network card driver yet
check dhcp        sh -c 'case "$(cat /proc/net/dhcp)" in *bound*10.0.2.16/24*) ;; *) cat /proc/net/dhcp; exit 1;; esac'
[ -n "$LIVE" ] ||   # live on real hardware: no network card driver yet
check resolv      sh -c 'case "$(cat /etc/resolv.conf)" in *"nameserver 10.0.2.3"*) ;; *) cat /etc/resolv.conf; exit 1;; esac'
check nc          sh -c '(nc -l 7007 >/tmp/nc.out &) ; sleep 1; echo "hello nc" | nc 127.0.0.1 7007; sleep 1; test "$(cat /tmp/nc.out)" = "hello nc"'
check ssh-key     sh -c 'rm -f /tmp/k && dropbearkey -t ed25519 -f /tmp/k >/dev/null && ssh -V 2>&1 | head -1 | test -n "$(cat)"'
[ -n "$LIVE" ] ||   # live on real hardware: no network card driver yet
check net-static  sh -c 'netcfg static 10.0.2.40/24 gateway 10.0.2.2 dns 10.0.2.3 && case "$(netcfg)" in *static*10.0.2.40/24*) ;; *) netcfg; exit 1;; esac && nettest >/dev/null'
[ -n "$LIVE" ] ||   # live on real hardware: no network card driver yet
check net-dhcp    sh -c 'netcfg dhcp && sleep 1 && case "$(netcfg)" in *bound*) ;; *) netcfg; exit 1;; esac'
check fsck-boot   sh -c 'cat /run/fsck-boot.log; read -r w r </run/fsck-boot.log && [ "$r" -le 3 ] &&
                         case " $(mount) " in *"/dev/root on / type ext2 (rw)"*) ;; *) mount; exit 1;; esac'
check e2fsprogs   sh -c 'dumpe2fs -h /dev/root 2>/dev/null | grep -q "^Filesystem magic number: *0xEF53" || exit 1
                         # read-only for the check: the free counts are written out (they are lazy
                         # while read-write) and nothing changes under e2fsck; its output waits in memory
                         mount -o remount,ro / || exit 1; out=$(e2fsck -fn /dev/root 2>&1); r=$?; mount -o remount,rw /
                         echo "$out"; echo "INFO e2fsck -fn on the whole disk: exit $r"; [ $r = 0 ]'
check remount     sh -c 'printf x | dd of=/dev/root bs=1 count=1 conv=notrunc 2>/dev/null && { echo "a raw write went through while read-write"; exit 1; }
                         mount -o remount,ro / && touch /tmp/ro-test 2>/dev/null && { echo "wrote while read-only"; mount -o remount,rw /; exit 1; }
                         mount -o remount,rw / && touch /tmp/ro-test && rm /tmp/ro-test'
check sshd        sh -c 'rm -f /tmp/k && dropbearkey -t ed25519 -f /tmp/k >/dev/null && mkdir -p /root/.ssh && chmod 700 /root/.ssh &&
                         dropbearkey -y -f /tmp/k | grep "^ssh-" > /root/.ssh/authorized_keys && chmod 600 /root/.ssh/authorized_keys
                         i=0; until s6-svstat -o up /run/service/sshd 2>/dev/null | grep -q true || [ $i -gt 50 ]; do sleep 0.1; i=$((i+1)); done
                         out=$(timeout 30 ssh -y -i /tmp/k root@127.0.0.1 "echo hello-\$((6*7)); id -u" </dev/null 2>&1)
                         echo "$out" | grep -q "^hello-42" && echo "$out" | grep -qx 0 || { echo "$out" | tail -5; exit 1; }'
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
[ -e /dev/linux ] && VM=1 || { VM=; say "(no Linux VM in this build: vm-bounds and linux-vm skipped)"; }
[ -z "$VM" ] ||
check vm-bounds   sh -c 'linux sh -c "i=0; until [ -e /mnt/sel4/etc/passwd ] || [ \$i -gt 100 ]; do sleep 0.2; i=\$((i+1)); done"
                         out=$(printf "linux id -u </dev/null\nlinux sh -c \"cat /mnt/sel4/etc/shadow >/dev/null 2>&1 && echo SHADOW-READ; echo SHADOW-TRIED\" </dev/null\nlinux sh -c \"echo x > /mnt/sel4/tmp/vm-user-file\" </dev/null\nexit\n" | login -f user 2>&1)
                         echo "$out" | grep -qx 1000 || { echo "the VM command did not run as uid 1000: $out"; exit 1; }
                         echo "$out" | grep -q SHADOW-TRIED || { echo "the shadow check did not run: $out"; exit 1; }
                         echo "$out" | grep -q SHADOW-READ && { echo "user read /etc/shadow through the VM"; exit 1; }
                         [ "$(stat -c %u /tmp/vm-user-file 2>&1)" = 1000 ] || { echo "a file the VM user created is not owned by 1000"; exit 1; }
                         rm -f /tmp/vm-user-file
                         [ "$(linux id -u)" = 0 ] || exit 1
                         linux sh -c "cat /mnt/sel4/etc/shadow" </dev/null >/dev/null 2>&1 && { echo "the VM root read /etc/shadow (no squashing)"; exit 1; }
                         true'
[ -z "$VM" ] ||
check linux-vm    sh -c 'test "$(linux echo hello from the VM)" = "hello from the VM" && linux sh -c "exit 3"; test $? = 3'
# The VM's system is on its own disk (Alpine and Docker, installed there with apk; the tests' copy
# is seeded with them) and Docker answers
[ -z "$VM" ] ||
check vm-docker   sh -c 'linux sh -c "grep -q \" / ext4 \" /proc/mounts" || { echo "the VM does not run from its disk: $(linux cat /proc/mounts)"; exit 1; }
                         i=0; until v=$(linux docker info --format "{{.ServerVersion}} {{.Driver}}" 2>/dev/null) && [ -n "$v" ]; do
                             sleep 0.5; i=$((i+1)); [ $i -le 60 ] || { echo "dockerd does not answer: $(linux tail -5 /var/log/dockerd.log)"; exit 1; }; done
                         echo "INFO docker $v"
                         case "$v" in *overlay2*) ;; *) echo "storage driver: $v"; exit 1;; esac
                         linux test -d /var/lib/docker/overlay2 || { echo "no /var/lib/docker/overlay2"; exit 1; }
                         out=$(printf "docker info --format DOCKER-AS-USER-OK </dev/null\nexit\n" | login -f user 2>&1)
                         echo "$out" | grep -q DOCKER-AS-USER-OK || { echo "a user cannot use docker (the docker group, gid 999): $(echo "$out" | tail -2)"; exit 1; }'
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

if [ -n "$LIVE" ]; then
    # Real hardware: what QEMU cannot tell (the numbers under QEMU are in TODO.md)
    say "== HARDWARE =="
    ms() { a=$(date +%s%N); "$@" >/dev/null 2>&1; b=$(date +%s%N); echo $(( (b - a) / 1000000 )); }
    say "uname: $(uname -a)"
    say "uptime at the end of the tests: $(cut -d' ' -f1 /proc/uptime) s"
    while IFS= read -r l; do say "  $l"; done < /proc/meminfo
    say "20 x fork+exec of /bin/true: $(ms sh -c 'i=0; while [ $i -lt 20 ]; do /bin/true; i=$((i+1)); done') ms"
    say "3000 shell loop iterations: $(ms sh -c 'i=0; while [ $i -lt 3000 ]; do i=$((i+1)); done') ms"
    head -c 8388608 /dev/zero > /tmp/hw.src
    say "8 MiB written to the RAM file system: $(ms cp /tmp/hw.src /tmp/hw.dst) ms"
    say "8 MiB read back: $(ms cat /tmp/hw.dst) ms"
    rm -f /tmp/hw.src /tmp/hw.dst
    say "Linux VM (VT-x on real hardware): first command after $(ms linux true) ms"
    say "  $(linux uname -a </dev/null 2>&1 | head -1)"
    linux sh -c 'grep -m1 "model name" /proc/cpuinfo; grep -c ^processor /proc/cpuinfo; free -m | head -2' </dev/null 2>&1 | \
        while IFS= read -r l; do say "  vm: $l"; done
    say "  10 x a VM command: $(ms sh -c 'i=0; while [ $i -lt 10 ]; do linux true </dev/null; i=$((i+1)); done') ms"
    say "  4 MiB VM -> native: $(ms sh -c 'linux head -c 4194304 /dev/zero </dev/null | wc -c') ms"
    say "  docker: $(linux docker version --format '{{.Server.Version}}' </dev/null 2>&1 | head -1)"
    say "== /proc/perf (count, cycles) =="
    grep -E '^(tsc_hz|sys\.(fork|execve|exit_group|read|write|openat)|mem\.|ext2\.(cache|disk|op\.read|op\.write))' /proc/perf | \
        while IFS= read -r l; do say "  $l"; done
    say "== END =="
    sync
    printf '\n%s\n' "Tests finished: $failures failures, $total tests. Press Enter to see the log as QR codes."
    read -r _ </dev/console
    qrlog $RESULTS </dev/console
    printf '%s\n' "The log is in $RESULTS (qrlog $RESULTS shows it again). Log in as user (you choose its password)."
    exit 0
fi

# Last: an orderly power off (make test checks that QEMU exits on its own and the disk is clean)
exec /sbin/poweroff
