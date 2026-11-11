#!/bin/bash
# Sandbox test of scripts/aa-bridge-apwatch.sh: fake hostapd pid, fake setup script. usage: tests/apwatch_test.sh   (prints "APWATCH ALL PASSED")
D=$(mktemp -d); cd "$(dirname "$0")/.." || exit 1; fail=0
chk() { if [ "$2" = ok ]; then echo "ok:   $1"; else echo "FAIL: $1"; fail=1; fi; }
cat > $D/setup.sh << EOT
#!/bin/sh
echo run >> $D/setup.calls
sleep 60 & echo \$! > $D/hostapd.pid     # "hostapd" is alive again after the rebuild
EOT
chmod +x $D/setup.sh
env_common="SETUP=$D/setup.sh PIDFILE=$D/hostapd.pid LOGF=$D/ap.log LOCKF=$D/lock GRACE=0 INTERVAL=0.2 BACKOFF=1 IFACE=nonexistent0 CHECK_IW=0"
# case 1: healthy hostapd -> never rebuilt
sleep 60 & echo $! > $D/hostapd.pid; HP=$!
env $env_common timeout 4 scripts/aa-bridge-apwatch.sh; n=$(wc -l < $D/setup.calls 2>/dev/null || echo 0)
[ "$n" = 0 ] && chk "healthy access point is left alone" ok || chk "healthy access point is left alone (setup ran $n x)" bad
# case 2: hostapd dies -> rebuilt once after 3 failed checks, then healthy again
kill $HP; wait $HP 2>/dev/null
env $env_common timeout 6 scripts/aa-bridge-apwatch.sh; n=$(wc -l < $D/setup.calls 2>/dev/null || echo 0)
[ "$n" = 1 ] && chk "dead hostapd -> access point rebuilt exactly once" ok || chk "dead hostapd -> rebuilt once (setup ran $n x)" bad
grep -q 'rebuilding it' $D/ap.log && chk "the rebuild is logged" ok || chk "the rebuild is logged" bad
# case 3: setup can't fix it -> gives up after MAXTRIES
rm -f $D/setup.calls; echo 99999999 > $D/hostapd.pid; printf '#!/bin/sh\necho run >> %s/setup.calls\n' $D > $D/setup.sh
env $env_common MAXTRIES=2 timeout 12 scripts/aa-bridge-apwatch.sh; n=$(wc -l < $D/setup.calls 2>/dev/null || echo 0)
[ "$n" = 2 ] && chk "gives up after MAXTRIES rebuilds (no endless restart loop)" ok || chk "gives up after 2 rebuilds (ran $n x)" bad
grep -q 'giving up' $D/ap.log && chk "giving up is logged" ok || chk "giving up is logged" bad
pkill -P $$ sleep 2>/dev/null; kill $(cat $D/hostapd.pid 2>/dev/null) 2>/dev/null; rm -rf $D
[ $fail = 0 ] && echo "APWATCH ALL PASSED" || echo "APWATCH FAILED"; exit $fail
