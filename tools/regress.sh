#!/bin/bash
# Full offline regression on the build host (run detached: nohup tools/regress.sh > /tmp/regress.out 2>&1 < /dev/null &). Prints one line per suite and DONE at the end.
cd /root/aa-bridge-build/src-aa-bridge/build-x || exit 1
make -j4 2>&1 | grep -E " error|undefined" | head -8
S=/root/aa-bridge-build/mnt/root; KK=/root/aa-certs/aa-proxy-keys; C=/root/aa-certs/current
export AA_NO_LAGGUARD=1 GST_REGISTRY_FORK=no GST_REGISTRY=/tmp/gst-reg-arm.bin GST_PLUGIN_SYSTEM_PATH=$S/usr/lib/aarch64-linux-gnu/gstreamer-1.0
Q="qemu-aarch64-static -L $S"
run() { local name=$1; shift; local o; o=$("$@" 2>&1 | grep -aE 'ALL PASSED|FAIL' | head -4 | tr '\n' ' '); [ -n "$o" ] || o="NO RESULT (timed out or crashed)"; echo "$name: $o"; }
run prefs    timeout 200 $Q ./prefs-test
run gest     timeout 200 $Q ./aa-compositor-test 1280 720 60 gest
run pipdrag  timeout 200 $Q ./aa-compositor-test 1280 720 60 pipdrag
run basecheck timeout 280 $Q ./aa-compositor-test 1280 720 60 basecheck 6
run idleskip  timeout 240 $Q ./aa-compositor-test 640 360 60 idleskip
run precond  timeout 200 $Q ./aa-compositor-test 1280 720 60 precond
run cornerlive timeout 250 $Q ./aa-compositor-test 1280 720 60 cornerlive
run reshape  timeout 200 $Q ./aa-compositor-test 1280 720 60 reshape
run winchange timeout 200 $Q ./aa-compositor-test 1280 720 60 winchange
run splitdrag timeout 200 $Q ./aa-compositor-test 1280 720 60 splitdrag
run fuzz7    timeout 400 $Q ./aa-compositor-test 1042 1080 60 fuzz 7 500
run fuzz9    timeout 400 $Q ./aa-compositor-test 1042 1080 60 fuzz 9 500
run loopback timeout 200 $Q ./hu-loopback-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run nav-e2e   timeout 120 $Q ./nav-e2e-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run av-e2e    timeout 150 $Q ./av-e2e-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run tls-order  timeout 200 $Q ./tls-order-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run btproxy timeout 150 $Q ./btproxy-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run sdreq timeout 150 $Q ./sdreq-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run obd timeout 60 $Q ./obd-test
run key-e2e timeout 150 $Q ./key-e2e-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run focus-e2e timeout 150 $Q ./focus-e2e-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run media-e2e timeout 150 $Q ./media-e2e-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run sensor-e2e timeout 150 $Q ./sensor-e2e-test $C/md_cert.pem $C/md_key.pem $KK/hu_cert.pem $KK/hu_key.pem
run hfp timeout 100 $Q ./hfp-test
run aacall timeout 60 $Q ./aacall-test
run callbridge timeout 120 $Q ./callbridge-test
run callsim timeout 90 $Q ./callsim-test
run silence  timeout 120 $Q ./silence-test $C/md_cert.pem $C/md_key.pem
echo "wifiwatch: $(cd .. && g++ -std=c++17 -pthread -Isrc tests/wifiwatch_test.cpp -o /tmp/wifiwatch_test && /tmp/wifiwatch_test 2>&1 | tail -1)"
echo "navrelay: $(cd .. && g++ -std=c++17 -pthread -Isrc tests/navrelay_test.cpp -o /tmp/navrelay_test && /tmp/navrelay_test 2>&1 | tail -1)"
echo "avrelay: $(cd .. && g++ -std=c++17 -pthread -Isrc tests/avrelay_test.cpp -o /tmp/avrelay_test && /tmp/avrelay_test 2>&1 | tail -1)"
echo "apwatch: $(cd .. && bash tests/apwatch_test.sh 2>&1 | tail -1)"
echo "msgring: $(cd .. && g++ -std=c++17 -pthread -Isrc tests/msgring_test.cpp -o /tmp/msgring_test && /tmp/msgring_test 2>&1 | tail -1)"
echo "watchdog: $(cd .. && g++ -std=c++17 -pthread -Isrc tests/watchdog_test.cpp -o /tmp/watchdog_test && /tmp/watchdog_test 2>&1 | tail -1)"
echo "bt: $(cd .. && timeout 250 python3 tests/bt_dbus_test.py all build-x/bt-dbus-test $S 2>&1 | tail -1)"
echo DONE
