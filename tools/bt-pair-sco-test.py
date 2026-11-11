#!/usr/bin/env python3
# Bench test for a USB Bluetooth dongle on the build host (needs bluez + python3-dbus + python3-gi): pairs adapter A to adapter B with an auto-accept agent, then opens an SCO link A->B and
# checks that call-audio packets reach the host on both sides. Usage: bt-pair-sco-test.py <addr A> <addr B>. Stale bonds on both adapters are removed first (they made the ATS2851 look broken).
# Auto-accept BlueZ agent (JustWorks / confirmation), then pair A -> B, then SCO test A -> B. Usage: pair_sco.py A B
import sys, time, threading, socket, subprocess
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib
A, B = sys.argv[1], sys.argv[2]
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
class Agent(dbus.service.Object):
    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Release(self): pass
    @dbus.service.method("org.bluez.Agent1", in_signature="ou", out_signature="")
    def RequestConfirmation(self, dev, code): print("agent: confirm", dev, code); return
    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="")
    def RequestAuthorization(self, dev): return
    @dbus.service.method("org.bluez.Agent1", in_signature="os", out_signature="")
    def AuthorizeService(self, dev, uuid): return
    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="u")
    def RequestPasskey(self, dev): return dbus.UInt32(0)
    @dbus.service.method("org.bluez.Agent1", in_signature="o", out_signature="s")
    def RequestPinCode(self, dev): return "0000"
    @dbus.service.method("org.bluez.Agent1", in_signature="ouq", out_signature="")
    def DisplayPasskey(self, dev, pk, entered): pass
    @dbus.service.method("org.bluez.Agent1", in_signature="", out_signature="")
    def Cancel(self): pass
agent = Agent(bus, "/test/agent")
am = dbus.Interface(bus.get_object("org.bluez", "/org/bluez"), "org.bluez.AgentManager1")
am.RegisterAgent("/test/agent", "NoInputNoOutput"); am.RequestDefaultAgent("/test/agent")
loop = GLib.MainLoop(); threading.Thread(target=loop.run, daemon=True).start()
om = dbus.Interface(bus.get_object("org.bluez", "/"), "org.freedesktop.DBus.ObjectManager")
def adapter_path(addr):
    for p, ifs in om.GetManagedObjects().items():
        if "org.bluez.Adapter1" in ifs and str(ifs["org.bluez.Adapter1"]["Address"]) == addr: return p
pa, pb = adapter_path(A), adapter_path(B); print("adapters", pa, pb)
for p in (pa, pb):
    props = dbus.Interface(bus.get_object("org.bluez", p), "org.freedesktop.DBus.Properties")
    props.Set("org.bluez.Adapter1", "Powered", True)
    for k in ("Pairable", "Discoverable"):
        try: props.Set("org.bluez.Adapter1", k, True)
        except Exception as e: print("set", k, "on", p, "->", e)

devpath = pa + "/dev_" + B.replace(":", "_")
# remove stale bonds on BOTH sides
for p, other in ((pa, B), (pb, A)):
    try: dbus.Interface(bus.get_object("org.bluez", p), "org.bluez.Adapter1").RemoveDevice(p + "/dev_" + other.replace(":", "_")); print("removed stale", p, other)
    except Exception as e: pass
time.sleep(1)
ad = dbus.Interface(bus.get_object("org.bluez", pa), "org.bluez.Adapter1")
ad.StartDiscovery(); t0 = time.time(); found = False
while time.time() - t0 < 15:
    if devpath in om.GetManagedObjects(): found = True; break
    time.sleep(0.5)
ad.StopDiscovery(); print("found", found)
if found:
    dev = dbus.Interface(bus.get_object("org.bluez", devpath), "org.bluez.Device1")
    try: dev.Pair(timeout=30); print("PAIR ok")
    except Exception as e: print("PAIR failed:", e)
    dp = dbus.Interface(bus.get_object("org.bluez", devpath), "org.freedesktop.DBus.Properties")
    print("Paired =", dp.Get("org.bluez.Device1", "Paired"))
    try: dp.Set("org.bluez.Device1", "Trusted", True)
    except Exception: pass
# SCO test
res = {}
def listener():
    s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_SCO); s.bind(B.encode()); s.listen(1); s.settimeout(25)
    try: c, addr = s.accept()
    except Exception as e: res['accept'] = 'FAILED %r' % (e,); return
    res['accept'] = 'ok'; c.settimeout(5); n = 0; sizes = set(); t0 = time.time(); echoed = 0
    try:
        while time.time() - t0 < 5:
            d = c.recv(1024); n += 1; sizes.add(len(d))
            if echoed < 50: c.send(d); echoed += 1
    except Exception as e: res['recv_err'] = repr(e)
    res['rx_packets'] = n; res['rx_sizes'] = sorted(sizes); c.close(); s.close()
th = threading.Thread(target=listener); th.start(); time.sleep(1)
c = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_SCO); c.bind(A.encode()); c.settimeout(20)
try:
    c.connect(B.encode()); res['connect'] = 'ok'; sent = 0; back = 0; bsz = set()
    c.settimeout(0.05)
    for i in range(400):
        try: c.send(bytes([i & 0xff]) * 48); sent += 1
        except Exception as e: res['send_err'] = repr(e); break
        try: d = c.recv(1024); back += 1; bsz.add(len(d))
        except Exception: pass
        time.sleep(0.005)
    res['sent'] = sent; res['echo_packets_back'] = back; res['echo_sizes'] = sorted(bsz)
except Exception as e: res['connect'] = 'FAILED %r' % (e,)
c.close(); th.join(30)
for k in sorted(res): print(k, '=', res[k])
