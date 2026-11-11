#!/usr/bin/env python3
"""Integration test for BtBootstrap against a fake BlueZ. Run on the build host (needs dbus-daemon, python3-dbus, qemu-aarch64-static and the ARM sysroot).

  tests/bt_dbus_test.py <scenario|all> <path to bt-dbus-test (aarch64)> <sysroot>
Scenarios:
  hang     ConnectProfile hangs (truck 28) and the phone connects 4 s into the hang: our NewConnection reply must still be prompt and the Wi-Fi handoff must complete.
  normal   ConnectProfile fails at once, phone connects: handoff completes.
  bounce   phone is Bluetooth-connected but never opens the AA Wireless profile: the link is bounced (Disconnect, then Connect) a few times, capped at 4.
"""
import os, re, subprocess, sys, time, tempfile, signal
HERE = os.path.dirname(os.path.abspath(__file__))
QEMU = 'qemu-aarch64-static'

def run(scn, binpath, sysroot):
    cfg = {
        'hang':   dict(fake=['--connect-profile', 'hang', '--hang-secs', '40', '--phone-at', '12'], secs=34, env={}),
        'normal': dict(fake=['--connect-profile', 'unavailable', '--phone-at', '6'], secs=18, env={}),
        'bounce': dict(fake=['--connect-profile', 'already', '--device-connected'], secs=30, env={'AA_BT_NUDGE_S': '1'}),
        'release': dict(fake=['--connect-profile', 'already', '--device-connected'], secs=24, env={'AA_BT_NUDGE_S': '1', 'AA_TEST_RELEASE': '1', 'AA_TEST_PHONE_ACTIVE': '1'}),
        'norelease': dict(fake=['--connect-profile', 'already', '--device-connected'], secs=20, env={'AA_BT_NUDGE_S': '1', 'AA_TEST_PHONE_ACTIVE': '1'}),
        'hfp':      dict(fake=['--connect-profile', 'ok', '--phone-at', '6'], secs=18, env={'AA_BT_NUDGE_S': '2', 'AA_TEST_PHONE_PROFILE': 'hfp'}),
        'hfpfallback': dict(fake=['--connect-profile', 'unavailable'], secs=12, env={'AA_BT_NUDGE_S': '1', 'AA_TEST_PHONE_PROFILE': 'hfp'}),
        'hfkeeper': dict(fake=['--connect-profile', 'ok', '--device-connected'], secs=14, env={'AA_BT_NUDGE_S': '1', 'AA_TEST_PHONE_PROFILE': 'hfp', 'AA_TEST_PHONE_ACTIVE': '1', 'AA_TEST_HF_MISSING': '1', 'AA_TEST_HF_KEEP_S': '3'}),
        'hsp':      dict(fake=['--connect-profile', 'ok'], secs=8, env={'AA_BT_NUDGE_S': '1'}),
    }[scn]
    d = tempfile.mkdtemp(prefix='btt-')
    bus = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--print-address'], stdout=subprocess.PIPE, text=True)
    addr = bus.stdout.readline().strip()
    env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=addr, DBUS_SESSION_BUS_ADDRESS=addr, AA_BT_NO_AP_WAIT='1', **cfg['env'])
    fake = subprocess.Popen([sys.executable, os.path.join(HERE, 'fake_bluez.py'), '--run-secs', str(cfg['secs'] + 8)] + cfg['fake'], env=env, stdout=open(d + '/fake.log', 'w'), stderr=subprocess.STDOUT)
    time.sleep(1.5)
    out = open(d + '/bt.log', 'w')
    app = subprocess.Popen([QEMU, '-L', sysroot, binpath, str(cfg['secs'])], env=env, stdout=out, stderr=subprocess.STDOUT)
    try: app.wait(timeout=cfg['secs'] + 60)
    except subprocess.TimeoutExpired: app.kill()
    time.sleep(0.5)
    for p in (fake, bus):
        p.send_signal(signal.SIGTERM)
        try: p.wait(timeout=5)
        except Exception: p.kill()
    return open(d + '/fake.log').read(), open(d + '/bt.log', errors='replace').read(), d

def check(name, cond, detail=''):
    print(('  ok   ' if cond else '  FAIL ') + name + (('  [' + detail + ']') if detail and not cond else '')); return bool(cond)

def main():
    which, binpath, sysroot = sys.argv[1], sys.argv[2], sys.argv[3]
    scns = ['hang', 'normal', 'bounce', 'release', 'norelease', 'hfp', 'hfpfallback', 'hfkeeper', 'hsp'] if which == 'all' else [which]
    bad = 0
    for s in scns:
        print('== scenario', s)
        fake, bt, d = run(s, binpath, sysroot)
        ok = check('client started and stopped cleanly', 'stopped cleanly' in bt, bt[-300:])
        if s in ('hang', 'normal'):
            m = re.search(r'NewConnection REPLY after ([\d.]+) s', fake)
            ok &= check('phone connection answered promptly (reply < 2 s)', bool(m) and float(m.group(1)) < 2.0, m.group(0) if m else 'no reply seen')
            ok &= check('Wi-Fi handoff completed (wireless bootstrap COMPLETE)', 'wireless bootstrap COMPLETE' in bt)
            ok &= check('fake phone finished the handshake', 'phone finished the handshake' in fake)
        if s == 'hang':
            ok &= check('the hanging ConnectProfile was reported as failed (client timeout)', re.search(r'ConnectProfile failed', bt) is not None)
            n = len(re.findall(r'ConnectProfile\(', fake))
            ok &= check('no pile-up of nudges while one is in flight (<= 2 calls in 34 s)', n <= 2, 'calls=%d' % n)
        if s == 'bounce':
            dis = len(re.findall(r'Disconnect called', fake)); con = len(re.findall(r'Connect called', fake))
            ok &= check('phone with no AA session is bounced (Disconnect + Connect seen)', dis >= 1 and con >= 1, 'disconnect=%d connect=%d' % (dis, con))
            ok &= check('bounces are capped at 4', dis <= 4, 'disconnect=%d' % dis)
        if s == 'release':
            dis = len(re.findall(r'Disconnect called', fake)); con = len(re.findall(r'Connect called', fake))
            ok &= check('phone with an active AA session is released after the delay (Disconnect seen, not bounced)', dis >= 1 and con == 0, 'disconnect=%d connect=%d' % (dis, con))
            ok &= check('release log line present', 'release: Android Auto runs over Wi-Fi' in bt)
            ok &= check('release is capped (<= 8)', dis <= 8, 'disconnect=%d' % dis)
        if s == 'norelease':
            dis = len(re.findall(r'Disconnect called', fake))
            ok &= check('with bt_release_phone off the phone link is left alone while an AA session is up', dis == 0, 'disconnect=%d' % dis)
        if s == 'hfp':
            ok &= check('phone woken over the HANDS-FREE profile (111f), never the headset one', 'ConnectProfile(0000111f' in fake and 'ConnectProfile(00001112' not in fake)
            ok &= check('Wi-Fi handoff still completes', 'wireless bootstrap COMPLETE' in bt)
        if s == 'hfpfallback':
            ok &= check('hands-free connect refused -> falls back to the headset profile', 'ConnectProfile(0000111f' in fake and 'ConnectProfile(00001112' in fake and 'waking it over the headset profile' in bt)
        if s == 'hfkeeper':
            n = len(re.findall(r'hands-free link missing while the session runs', bt))
            ok &= check('session up without a hands-free link -> the keeper connects hands-free (repeats)', n >= 2 and 'ConnectProfile(0000111f' in fake, 'keeper=%d' % n)
            ok &= check('no headset nudges while the session runs', 'ConnectProfile(00001112' not in fake)
        if s == 'hsp':
            ok &= check('default (no call path): headset wake-up as before', 'ConnectProfile(00001112' in fake and 'ConnectProfile(0000111f' not in fake)
        if not ok: bad += 1; print('---- fake log tail:\n' + fake[-1500:] + '\n---- client log tail:\n' + bt[-1800:])
        print('   logs:', d)
    print('ALL PASSED' if not bad else '%d SCENARIO(S) FAILED' % bad)
    return 1 if bad else 0

if __name__ == '__main__': sys.exit(main())
