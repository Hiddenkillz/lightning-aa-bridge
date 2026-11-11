#!/usr/bin/env python3
"""Fake BlueZ for tests/bt_dbus_test.sh: just enough of org.bluez on a PRIVATE bus (DBUS_SYSTEM_BUS_ADDRESS) to exercise src/headunit/BtBootstrap.cpp.

  --connect-profile hang|unavailable|already|ok   what Device1.ConnectProfile(HSP AG) does ("hang": never answers in time, like truck 28)
  --hang-secs N          how long "hang" holds the call before answering (the client gives up after 15 s)
  --phone-at SECS        at this time the fake phone connects: Connected=true + Profile1.NewConnection on the AA Wireless profile (-1: never)
  --device-connected     the paired phone is already Connected at start (Bluetooth up, but it never opens the AA Wireless profile)
Everything the client does is timestamped on stdout so the shell test can assert on it.
"""
import argparse, os, socket, struct, sys, threading, time
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib

ap = argparse.ArgumentParser()
ap.add_argument('--connect-profile', default='unavailable'); ap.add_argument('--hang-secs', type=float, default=40)
ap.add_argument('--phone-at', type=float, default=-1); ap.add_argument('--device-connected', action='store_true')
ap.add_argument('--run-secs', type=float, default=60)
A = ap.parse_args()
T0 = time.time()
def say(msg): print('[fake t=%6.2f] %s' % (time.time() - T0, msg), flush=True)

dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
bus = dbus.SystemBus()
BN = dbus.service.BusName('org.bluez', bus)
DEV = '/org/bluez/hci0/dev_11_22_33_44_55_66'
state = {'connected': A.device_connected, 'aaw': None, 'client': None}

def adapter_props(): return {'Address': '00:11:22:33:44:55', 'Powered': True, 'Alias': 'fake', 'Discoverable': True, 'Pairable': True}
def device_props():
    return {'Address': '66:55:44:33:22:11', 'Alias': 'Fake Pixel', 'Paired': True, 'Trusted': True, 'Connected': state['connected'], 'ServicesResolved': state['connected'],
            'UUIDs': dbus.Array(['0000111f-0000-1000-8000-00805f9b34fb', '00001112-0000-1000-8000-00805f9b34fb', '4de17a00-52cb-11e6-bdf4-0800200c9a66'], signature='s')}

class Root(dbus.service.Object):
    @dbus.service.method('org.freedesktop.DBus.ObjectManager', out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        return {dbus.ObjectPath('/org/bluez/hci0'): {'org.bluez.Adapter1': adapter_props()}, dbus.ObjectPath(DEV): {'org.bluez.Device1': device_props()}}
class Org(dbus.service.Object):
    @dbus.service.method('org.bluez.ProfileManager1', in_signature='osa{sv}', sender_keyword='sender')
    def RegisterProfile(self, path, uuid, opts, sender=None):
        say('RegisterProfile %s %s' % (path, uuid))
        if str(uuid).startswith('4de17a00'): state['aaw'] = str(path); state['client'] = sender
    @dbus.service.method('org.bluez.AgentManager1', in_signature='os')
    def RegisterAgent(self, p, c): pass
    @dbus.service.method('org.bluez.AgentManager1', in_signature='o')
    def RequestDefaultAgent(self, p): pass
class Hci(dbus.service.Object):
    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ssv')
    def Set(self, i, p, v): pass
    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, i, p): return adapter_props().get(str(p), '')
class Device(dbus.service.Object):
    @dbus.service.method('org.bluez.Device1', in_signature='s', async_callbacks=('ok', 'err'))
    def ConnectProfile(self, uuid, ok, err):
        say('ConnectProfile(%s) called [%s]' % (uuid, A.connect_profile))
        m = A.connect_profile
        if m == 'hang': GLib.timeout_add(int(A.hang_secs * 1000), lambda: (err(dbus.exceptions.DBusException('too late', name='org.bluez.Error.Failed')), False)[1]); return
        if m == 'unavailable': err(dbus.exceptions.DBusException('br-connection-profile-unavailable', name='org.bluez.Error.NotAvailable')); return
        if m == 'already': err(dbus.exceptions.DBusException('br-connection-already-connected', name='org.bluez.Error.Failed')); return
        ok()
    @dbus.service.method('org.bluez.Device1')
    def Disconnect(self): say('Disconnect called'); state['connected'] = False
    @dbus.service.method('org.bluez.Device1')
    def Connect(self): say('Connect called'); state['connected'] = True
    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, i, p): return device_props().get(str(p), '')

Root(bus, '/'); Org(bus, '/org/bluez'); Hci(bus, '/org/bluez/hci0'); Device(bus, DEV)

def frame(i, body=b''): return struct.pack('>HH', len(body), i) + body
def phone_thread(sock):                                     # the phone side of the wireless-AA Wi-Fi handoff (same script as tests/bt_bootstrap_test.cpp)
    def rd():
        h = b''
        while len(h) < 4:
            c = sock.recv(4 - len(h));
            if not c: return None, None
            h += c
        n, i = struct.unpack('>HH', h); b = b''
        while len(b) < n: b += sock.recv(n - len(b))
        return i, b
    try:
        i, _ = rd(); say('phone got message id %s (WifiStartRequest expected 1)' % i)
        sock.sendall(frame(2)); i, _ = rd(); say('phone got message id %s (WifiInfoResponse expected 3)' % i)
        sock.sendall(frame(7, b'\x08\x00')); sock.sendall(frame(6, b'\x08\x00')); say('phone finished the handshake'); time.sleep(0.3)
    except Exception as e: say('phone thread error %s' % e)
    sock.close()

def phone_connects():
    say('PHONE CONNECTS: Connected=true, opening the AA Wireless profile')
    state['connected'] = True
    a, b = socket.socketpair(); threading.Thread(target=phone_thread, args=(a,), daemon=True).start()
    t = time.time(); prof = bus.get_object(state['client'], state['aaw'], introspect=False)
    iface = dbus.Interface(prof, 'org.bluez.Profile1')
    iface.NewConnection(dbus.ObjectPath(DEV), dbus.types.UnixFd(b.fileno()), dbus.Dictionary({}, signature='sv'), timeout=40,
                        reply_handler=lambda: say('NewConnection REPLY after %.2f s' % (time.time() - t)), error_handler=lambda e: say('NewConnection ERROR after %.2f s: %s' % (time.time() - t, e)))
    return False

if A.phone_at >= 0: GLib.timeout_add(int(A.phone_at * 1000), phone_connects)
GLib.timeout_add(int(A.run_secs * 1000), lambda: (loop.quit(), False)[1])
say('fake bluez up (connect-profile=%s, phone-at=%s, device-connected=%s)' % (A.connect_profile, A.phone_at, A.device_connected))
loop = GLib.MainLoop(); loop.run()
