#!/usr/bin/env python3
"""Desk test of `aarecvd --bt-only` (pi/aarecvd/src/Bluetooth).

A pretend BlueZ (org.bluez on a private D-Bus daemon: profile and agent managers, the adapter hci0, a paired phone,
a paired keyboard and an unpaired phone) stands in for bluetoothd; the program under test gets that bus as its
system bus (DBUS_SYSTEM_BUS_ADDRESS). It has to match the recorded reference behaviour for this
scenario (REFERENCE below): the profiles and agent it registers, what it sets on the adapter, the
start-up messages it sends the phone (byte for byte), what it refuses, when it connects, nudges, gives up, and how it
exits. Then pairing from aaview's icon.

  python3 aarecvd_bt_selftest.py [path/to/aarecvd]

Runs in the veloaa-aarecvd-build:amd64 image (build.sh bttest): it writes /etc/aarecv.conf and /etc/aarecv/loglevel
there. Not for the Pi: it would overwrite those two files. The chip-reset step is pointed at a device that does not
exist.
"""
import os, signal, socket, struct, subprocess, sys, tempfile, threading, time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import GLib

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)
import aapb  # noqa: E402  (pb / pv, recvn, the UUIDs and the port)

# The recorded reference behaviour for this scenario. main() compares aarecvd against it.
REFERENCE = {
    'registration': [
        ('RegisterProfile', '/veloaa/aaw', '4de17a00-52cb-11e6-bdf4-0800200c9a66',
         {'Name': ('String', 'AA Wireless'), 'Role': ('String', 'server'), 'Channel': ('UInt16', 22),
          'RequireAuthentication': ('Boolean', False), 'RequireAuthorization': ('Boolean', False)}),
        ('RegisterProfile', '/veloaa/hsp', '00001108-0000-1000-8000-00805f9b34fb',
         {'Name': ('String', 'HSP HS'), 'RequireAuthentication': ('Boolean', False),
          'RequireAuthorization': ('Boolean', False)}),
        ('RegisterAgent', '/veloaa/agent', 'DisplayYesNo'),
        ('RequestDefaultAgent', '/veloaa/agent')],
    'adapter_sets': [('Set', '/org/bluez/hci0', 'Powered', 'Boolean', 1),
                     ('Set', '/org/bluez/hci0', 'Pairable', 'Boolean', 0)],
    'agent': {'RequestConfirmation': 'org.bluez.Error.Rejected', 'RequestPinCode': 'org.bluez.Error.Rejected',
              'RequestAuthorization': 'org.bluez.Error.Rejected', 'AuthorizeService (paired)': 'ok',
              'AuthorizeService (unpaired)': 'org.bluez.Error.Rejected', 'Cancel': 'ok'},
    'wifi_start': (1, b'\n\t10.42.0.1\x10\xa8)'),
    'wifi_info': (3, b'\n\x0bVeloAA-test\x12\x0ctest-key-123\x1a\x1102:00:00:00:00:01 \x08(\x01'),
    'first_connects': [('/org/bluez/hci0/dev_11_22_33_44_55_66', '00001112-0000-1000-8000-00805f9b34fb')],
    'nudge': (True, True),
    'inprogress': (0, True),
    'noreply_exit': 3,
    'pair': {'adapter_sets': [('Set', '/org/bluez/hci0', 'Powered', 'Boolean', 1),
                              ('Set', '/org/bluez/hci0', 'Pairable', 'Boolean', 1),
                              ('Set', '/org/bluez/hci0', 'DiscoverableTimeout', 'UInt32', 180),
                              ('Set', '/org/bluez/hci0', 'Discoverable', 'Boolean', 1)],
             'agent': {'RequestConfirmation': 'ok', 'RequestPinCode': ('ok', '0000'), 'RequestPasskey': ('ok', 0)},
             'trusted': [('Set', '/org/bluez/hci0/dev_22_33_44_55_66_77', 'Trusted', 'Boolean', 1)]},
}

PHONE = '/org/bluez/hci0/dev_11_22_33_44_55_66'
KBD = '/org/bluez/hci0/dev_AA_BB_CC_DD_EE_01'
STRANGER = '/org/bluez/hci0/dev_22_33_44_55_66_77'
SECOND = '/org/bluez/hci0/dev_33_44_55_66_77_88'
AP = dict(ssid='VeloAA-test', key='test-key-123', bssid='02:00:00:00:00:01', ip='10.42.0.1')

RESULTS = []
RUNS = []   # every Run, kept: dbus-python breaks (heap corruption) when a connection is freed while the GLib thread
            # still dispatches on the bus


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok)))
    print('  %s  %s%s' % ('ok  ' if ok else 'FAIL', name, ('   (%s)' % (detail,)) if detail and not ok else ''),
          flush=True)
    return ok


# ---------------- the pretend BlueZ ----------------

class Rejected(dbus.DBusException):
    _dbus_error_name = 'org.bluez.Error.Rejected'


class InProgress(dbus.DBusException):
    _dbus_error_name = 'org.bluez.Error.InProgress'


class NoReply(dbus.DBusException):
    _dbus_error_name = 'org.freedesktop.DBus.Error.NoReply'


class Bluez:
    def __init__(self, bus):
        self.bus, self.calls, self.lock = bus, [], threading.Lock()
        self.adapter = {'Address': 'AA:BB:CC:00:11:22', 'Alias': 'velosterAA-test', 'Powered': dbus.Boolean(False),
                        'Pairable': dbus.Boolean(True), 'Discoverable': dbus.Boolean(False),
                        'DiscoverableTimeout': dbus.UInt32(0)}
        self.devices = {
            PHONE: {'Address': '11:22:33:44:55:66', 'Alias': 'Test Phone', 'Icon': 'phone', 'Paired': True,
                    'Connected': False, 'Trusted': False},
            KBD: {'Address': 'AA:BB:CC:DD:EE:01', 'Alias': 'Keyboard', 'Icon': 'input-keyboard', 'Paired': True,
                  'Connected': False, 'Trusted': True},
            STRANGER: {'Address': '22:33:44:55:66:77', 'Alias': 'Stranger', 'Icon': 'phone', 'Paired': False,
                       'Connected': False, 'Trusted': False},
        }
        self.mode = 'ok'            # how ConnectProfile answers: ok / inprogress / noreply
        self.client = None          # the program under test's unique bus name
        Root(self)
        Manager(self)
        Props(self, '/org/bluez/hci0', 'org.bluez.Adapter1', self.adapter)
        for path, d in self.devices.items():
            Device(self, path, d)

    def record(self, *call):
        with self.lock:
            self.calls.append((time.time(),) + call)

    def since(self, t, name=None):
        with self.lock:
            return [c for c in self.calls if c[0] >= t and (name is None or c[1] == name)]


class Root(dbus.service.Object):
    def __init__(self, bz):
        dbus.service.Object.__init__(self, bz.bus, '/')
        self.bz = bz

    @dbus.service.method('org.freedesktop.DBus.ObjectManager', in_signature='', out_signature='a{oa{sa{sv}}}')
    def GetManagedObjects(self):
        out = {'/org/bluez/hci0': {'org.bluez.Adapter1': dict(self.bz.adapter)}}
        for path, d in self.bz.devices.items():
            out[path] = {'org.bluez.Device1': {
                'Address': d['Address'], 'Alias': d['Alias'], 'Icon': d['Icon'], 'Paired': dbus.Boolean(d['Paired']),
                'Connected': dbus.Boolean(d['Connected']), 'Trusted': dbus.Boolean(d['Trusted']),
                'RSSI': dbus.Int16(-60), 'UUIDs': dbus.Array([aapb.HSP_AG_UUID], signature='s')},
                'org.freedesktop.DBus.Introspectable': {}}
        return out


class Manager(dbus.service.Object):
    def __init__(self, bz):
        dbus.service.Object.__init__(self, bz.bus, '/org/bluez')
        self.bz = bz

    @dbus.service.method('org.bluez.ProfileManager1', in_signature='osa{sv}', out_signature='',
                         sender_keyword='sender')
    def RegisterProfile(self, path, uuid, opts, sender=None):
        self.bz.client = sender
        self.bz.record('RegisterProfile', str(path), str(uuid),
                       {str(k): (type(v).__name__, v if isinstance(v, (bool, int)) else str(v)) for k, v in opts.items()})

    @dbus.service.method('org.bluez.AgentManager1', in_signature='os', out_signature='')
    def RegisterAgent(self, path, capability):
        self.bz.record('RegisterAgent', str(path), str(capability))

    @dbus.service.method('org.bluez.AgentManager1', in_signature='o', out_signature='')
    def RequestDefaultAgent(self, path):
        self.bz.record('RequestDefaultAgent', str(path))


class Props(dbus.service.Object):
    def __init__(self, bz, path, iface, values):
        dbus.service.Object.__init__(self, bz.bus, path)
        self.bz, self.path, self.iface, self.values = bz, path, iface, values

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ss', out_signature='v')
    def Get(self, iface, name):
        v = self.values[str(name)]
        return dbus.Boolean(v) if isinstance(v, bool) else v

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='ssv', out_signature='')
    def Set(self, iface, name, value):
        self.bz.record('Set', self.path, str(name), type(value).__name__, int(value))
        self.values[str(name)] = value

    @dbus.service.method('org.freedesktop.DBus.Properties', in_signature='s', out_signature='a{sv}')
    def GetAll(self, iface):
        return dict(self.values)


class Device(Props):
    def __init__(self, bz, path, values):
        Props.__init__(self, bz, path, 'org.bluez.Device1', values)

    @dbus.service.method('org.bluez.Device1', in_signature='s', out_signature='')
    def ConnectProfile(self, uuid):
        self.bz.record('ConnectProfile', self.path, str(uuid), self.bz.mode)
        if self.bz.mode == 'inprogress':
            raise InProgress('in progress')
        if self.bz.mode == 'noreply':
            raise NoReply('no reply')

    @dbus.service.method('org.bluez.Device1', in_signature='', out_signature='')
    def Disconnect(self):
        self.bz.record('Disconnect', self.path)


# ---------------- helpers ----------------

def frame(mid, body=b''):
    return struct.pack('>HH', len(body), mid) + body


def read_frame(s, timeout=5.0):
    s.settimeout(timeout)
    hdr = aapb.recvn(s, 4)
    n, mid = struct.unpack('>HH', hdr)
    return mid, aapb.recvn(s, n)


def eof_within(s, seconds):
    """Seconds until the other side closed s, or None if it is still open after `seconds`."""
    t0 = time.time()
    s.settimeout(seconds)
    try:
        while True:
            if not s.recv(64):
                return time.time() - t0
    except socket.timeout:
        return None
    except OSError:
        return time.time() - t0


def wait_for(cond, seconds, step=0.1):
    end = time.time() + seconds
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(step)
    return cond()


class Run:
    """aarecvd --bt-only against the pretend BlueZ."""

    def __init__(self, kind, binary, bz, address, tmp, pair=False):
        self.kind, self.bz, self.tmp = kind, bz, tmp
        self.sock = os.path.join(tmp, 'aarecv-%s.sock' % kind)
        self.logpath = os.path.join(tmp, 'log-%s%s.txt' % (kind, '-pair' if pair else ''))
        self.pairfile = os.path.join(tmp, 'pairing-%s' % kind)
        self.joinfile = os.path.join(tmp, 'joining-%s' % kind)
        cmd = [binary, '--bt-only', '--notify-socket', self.sock, '--ap-conf', '/etc/aarecv.conf',
               '--loglevel-file', '/etc/aarecv/loglevel', '--settings', os.path.join(tmp, 'none'),
               '--bt-serial-device', 'veloaa-test-no-such-device', '--pairing-file', self.pairfile,
               '--joining-file', self.joinfile]
        if pair:
            cmd.append('--pair')
        env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS=address)
        env.pop('JOURNAL_STREAM', None)
        self.t0 = time.time()
        self.p = subprocess.Popen(cmd, stdout=open(self.logpath, 'w'), stderr=subprocess.STDOUT, env=env)
        self.ctl = dbus.bus.BusConnection(address)   # a second connection, for blocking calls from this thread
        RUNS.append(self)

    def log(self):
        return open(self.logpath, errors='replace').read()

    def logged(self, text, seconds=5.0):
        return wait_for(lambda: text in self.log(), seconds)

    def rss_kb(self):
        try:
            for ln in open('/proc/%d/status' % self.p.pid):
                if ln.startswith('VmRSS:'):
                    return int(ln.split()[1])
        except OSError:
            pass
        return 0

    def obj(self, path, iface):
        return dbus.Interface(self.ctl.get_object(self.bz.client, path), iface)

    def new_connection(self, profile, device):
        a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        b.setblocking(False)        # as bluetoothd hands the RFCOMM socket over
        self.obj('/veloaa/' + profile, 'org.bluez.Profile1').NewConnection(
            dbus.ObjectPath(device), dbus.types.UnixFd(b.fileno()), dbus.Dictionary({}, signature='sv'))
        b.close()                   # the program has its own copy now
        return a

    def agent(self, method, *args):
        try:
            r = getattr(self.obj('/veloaa/agent', 'org.bluez.Agent1'), method)(*args)
            return 'ok', r
        except dbus.DBusException as ex:
            return ex.get_dbus_name(), None

    def pairing(self):
        """The pairing file as (until - now, passkey), or None."""
        try:
            d = dict(ln.split() for ln in open(self.pairfile) if ln.strip())
            return float(d['until']) - time.monotonic(), int(d['passkey'])
        except (OSError, ValueError, KeyError):
            return None

    def notify(self, text):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        s.sendto(text.encode(), self.sock)
        s.close()

    def stop(self):
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try:
                self.p.wait(5)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()
        return self.p.returncode


# ---------------- the scenario ----------------

def scenario(kind, binary, bz, address, tmp):
    """Returns what was seen, for comparing with the recorded reference."""
    seen = {}
    print('\n== aarecvd --bt-only', flush=True)
    bz.devices[PHONE]['Connected'] = False
    bz.mode = 'ok'
    start = time.time()
    r = Run(kind, binary, bz, address, tmp)
    try:
        if not check('%s: registers with BlueZ and listens on the notify socket' % kind,
                     wait_for(lambda: bz.since(start, 'RequestDefaultAgent'), 15) and r.logged('listening to it on', 5),
                     r.log()[-400:]):
            return seen
        seen['rss_kb'] = r.rss_kb()
        reg = [c[1:] for c in bz.since(start) if c[1] in ('RegisterProfile', 'RegisterAgent', 'RequestDefaultAgent')]
        seen['registration'] = reg
        sets = [c[1:] for c in bz.since(start, 'Set')]
        seen['adapter_sets'] = sets
        aaw = [c for c in reg if c[0] == 'RegisterProfile' and c[2] == aapb.AAW_UUID]
        check('%s: AA wireless profile: server, RFCOMM channel 22 (UInt16), no authentication or authorization'
              % kind, aaw and aaw[0][1] == '/veloaa/aaw' and aaw[0][3].get('Channel') == ('UInt16', 22)
              and aaw[0][3].get('Role') == ('String', 'server')
              and aaw[0][3].get('RequireAuthentication') == ('Boolean', 0)
              and aaw[0][3].get('RequireAuthorization') == ('Boolean', 0), aaw)
        hsp = [c for c in reg if c[0] == 'RegisterProfile' and c[2] == aapb.HSP_HS_UUID]
        check('%s: HSP headset profile' % kind, hsp and hsp[0][1] == '/veloaa/hsp', hsp)
        check('%s: agent DisplayYesNo, made the default' % kind,
              ('RegisterAgent', '/veloaa/agent', 'DisplayYesNo') in reg and ('RequestDefaultAgent', '/veloaa/agent') in reg,
              reg)
        check('%s: adapter powered, not pairable, not made discoverable' % kind,
              ('Set', '/org/bluez/hci0', 'Powered', 'Boolean', 1) in sets
              and ('Set', '/org/bluez/hci0', 'Pairable', 'Boolean', 0) in sets
              and not any(s[2] == 'Discoverable' for s in sets), sets)
        check('%s: logs the adapter address' % kind, r.logged('bt: profiles registered, adapter AA:BB:CC:00:11:22'))

        # agent without --pair
        refusals = {
            'RequestConfirmation': r.agent('RequestConfirmation', dbus.ObjectPath(PHONE), dbus.UInt32(123456))[0],
            'RequestPinCode': r.agent('RequestPinCode', dbus.ObjectPath(PHONE))[0],
            'RequestAuthorization': r.agent('RequestAuthorization', dbus.ObjectPath(PHONE))[0],
            'AuthorizeService (paired)': r.agent('AuthorizeService', dbus.ObjectPath(PHONE), aapb.AAW_UUID)[0],
            'AuthorizeService (unpaired)': r.agent('AuthorizeService', dbus.ObjectPath(STRANGER), aapb.AAW_UUID)[0],
            'Cancel': r.agent('Cancel')[0],
        }
        seen['agent'] = refusals
        rej = 'org.bluez.Error.Rejected'
        check('%s: without --pair, pairing is refused; a paired phone\'s service is authorized' % kind,
              refusals == {'RequestConfirmation': rej, 'RequestPinCode': rej, 'RequestAuthorization': rej,
                           'AuthorizeService (paired)': 'ok', 'AuthorizeService (unpaired)': rej, 'Cancel': 'ok'},
              refusals)
        check('%s: the refusal is logged' % kind,
              r.logged('refused (pairing is only open with --pair or a 3 s hold on the icon)'))

        # an unpaired device on the AA wireless profile
        s = r.new_connection('aaw', STRANGER)
        t = eof_within(s, 3)
        s.close()
        check('%s: AA wireless link from an unpaired device: closed at once, nothing sent' % kind,
              t is not None and r.logged('which is not paired: closed'), t)

        # the headset link, then the AA wireless link: the headset link is dropped at once
        h = r.new_connection('hsp', PHONE)
        joined = os.path.exists(r.joinfile)
        a = r.new_connection('aaw', PHONE)
        check('%s: the AA wireless link touches the joining file (for aa-ap), the headset link does not' % kind,
              not joined and wait_for(lambda: os.path.exists(r.joinfile), 2), joined)
        t = eof_within(h, 2)
        h.close()
        check('%s: the AA wireless link drops the headset link at once' % kind, t is not None and t < 1.5, t)
        mid, body = read_frame(a)
        want = aapb.pb(1, AP['ip']) + aapb.pv(2, aapb.PORT)
        seen['wifi_start'] = (mid, body)
        check('%s: WifiStartRequest (1): ip of the access point, port 5288' % kind, mid == 1 and body == want,
              (mid, body))
        a.sendall(frame(2))
        mid, body = read_frame(a)
        want = (aapb.pb(1, AP['ssid']) + aapb.pb(2, AP['key']) + aapb.pb(3, AP['bssid']) + aapb.pv(4, 8)
                + aapb.pv(5, 1))
        seen['wifi_info'] = (mid, body)
        check('%s: WifiInfoRequest -> WifiInfoResponse (3): ssid, key, bssid, WPA2 personal (8), dynamic (1)' % kind,
              mid == 3 and body == want, (mid, body))
        a.sendall(frame(7, aapb.pv(1, 0)))
        check('%s: WifiStartResponse logged with its fields' % kind, r.logged('WifiStartResponse (7) {1: [0]}'))
        a.close()
        check('%s: the phone closing the link ends the exchange' % kind,
              r.logged('bt: bootstrap ended: connection closed'))

        # a headset link with no session: held 10 s (in the background while the rest goes on)
        hold = {}
        h10 = r.new_connection('hsp', PHONE)

        def held():
            hold['t'] = eof_within(h10, 14)
            h10.close()
        th = threading.Thread(target=held)
        th.start()

        # auto-connect: about 10 s after start, the paired phone only, with the headset (audio gateway) profile
        conns = wait_for(lambda: bz.since(start, 'ConnectProfile'), 15)
        seen['first_connects'] = sorted(set((c[2], c[3]) for c in conns or []))
        first = conns[0][0] - start if conns else None
        check('%s: auto-connect after about 10 s, to the paired phone only, profile HSP AG' % kind,
              conns and all(c[2] == PHONE and c[3] == aapb.HSP_AG_UUID for c in conns) and 8 < first < 13,
              (first, seen['first_connects']))
        check('%s: "connecting to Test Phone (11:22:33:44:55:66)"' % kind,
              r.logged('bt: connecting to Test Phone (11:22:33:44:55:66)'))
        th.join()
        check('%s: headset link with no session closed after 10 s' % kind,
              hold.get('t') is not None and 9 < hold['t'] < 12, hold)

        # during a session: no connects, and a headset link is closed within about a second
        r.notify('session up')
        check('%s: "session up" from aarecvd logged' % kind, r.logged('aa: aarecvd: session up'))
        h = r.new_connection('hsp', PHONE)
        t = eof_within(h, 4)
        h.close()
        check('%s: during a session a headset link is closed after 1 s' % kind, t is not None and 0.5 < t < 2.5, t)
        mark = time.time()
        time.sleep(11)
        check('%s: no auto-connect during a session (11 s)' % kind, not bz.since(mark, 'ConnectProfile'),
              bz.since(mark, 'ConnectProfile'))
        r.notify('icon 1')          # ignored during a session (only logged at debug)
        time.sleep(0.5)
        check('%s: an icon tap during a session does not connect' % kind, not bz.since(mark, 'ConnectProfile'))
        r.notify('session ended')
        check('%s: "session ended" logged' % kind, r.logged('aa: aarecvd: session ended'))

        # icon tap with the phone connected but no session: nudged at once (disconnect, connect 2 s later)
        bz.devices[PHONE]['Connected'] = True
        mark = time.time()
        r.notify('icon 1')
        disc = wait_for(lambda: bz.since(mark, 'Disconnect'), 3)
        conn = wait_for(lambda: [c for c in bz.since(mark, 'ConnectProfile') if disc and c[0] > disc[0][0]], 5)
        gap = conn[0][0] - disc[0][0] if disc and conn else None
        seen['nudge'] = (bool(disc), bool(conn))
        check('%s: icon tap with no session nudges a connected phone: disconnect, then connect 2 s later' % kind,
              disc and disc[0][2] == PHONE and conn and 1.5 < gap < 3.5, (disc, conn, gap))
        check('%s: "icon tapped (state 1)" and "nudging" logged' % kind,
              r.logged('icon tapped (state 1) with no phone session: connecting to the phone now')
              and r.logged('(nudge 1)'))
        bz.devices[PHONE]['Connected'] = False
        time.sleep(3)

        # "in progress": once may be the phone connecting at the same moment; twice running, the link is reset.
        # Start just after a 10 s auto-connect, so that one does not land in the middle.
        tick = time.time()
        wait_for(lambda: bz.since(tick, 'ConnectProfile'), 12)
        bz.mode = 'inprogress'
        mark = time.time()
        r.notify('icon 1')
        wait_for(lambda: len(bz.since(mark, 'ConnectProfile')) >= 1, 3)
        time.sleep(0.5)
        one = bz.since(mark, 'Disconnect')
        r.notify('icon 1')
        wait_for(lambda: len(bz.since(mark, 'ConnectProfile')) >= 2, 3)
        two = wait_for(lambda: bz.since(mark, 'Disconnect'), 2)
        seen['inprogress'] = (len(one), bool(two))
        check('%s: one "in progress" leaves the link alone; the second resets it' % kind, not one and two,
              (one, two))

        # NoReply three times in 5 minutes, and the chip does not answer (there is none here): reset and exit 3
        bz.mode = 'noreply'
        time.sleep(0.3)
        for _ in range(3):
            if r.p.poll() is not None:
                break
            n = len(bz.since(mark, 'ConnectProfile'))
            r.notify('icon 1')
            wait_for(lambda: len(bz.since(mark, 'ConnectProfile')) > n or r.p.poll() is not None, 4)
            time.sleep(0.5)
        try:
            code = r.p.wait(15)
        except subprocess.TimeoutExpired:
            code = None
        seen['noreply_exit'] = code
        check('%s: three NoReply and a silent chip: "chip is hung", reset attempted, exit 3' % kind,
              code == 3 and r.logged('the Bluetooth chip is hung') and r.logged('bt: reset failed'),
              (code, r.log()[-300:]))
    finally:
        r.stop()
    print('  (%s log: %s)' % (kind, r.logpath))
    return seen


def scenario_pair(kind, binary, bz, address, tmp):
    seen = {}
    bz.mode = 'ok'
    start = time.time()
    r = Run(kind, binary, bz, address, tmp, pair=True)
    try:
        check('%s --pair: registers' % kind, wait_for(lambda: bz.since(start, 'RequestDefaultAgent'), 15)
              and r.logged('listening to it on', 5))
        sets = [c[1:] for c in bz.since(start, 'Set')]
        seen['adapter_sets'] = sets
        check('%s --pair: pairable, discoverable for 180 s' % kind,
              ('Set', '/org/bluez/hci0', 'Pairable', 'Boolean', 1) in sets
              and ('Set', '/org/bluez/hci0', 'DiscoverableTimeout', 'UInt32', 180) in sets
              and ('Set', '/org/bluez/hci0', 'Discoverable', 'Boolean', 1) in sets, sets)
        check('%s --pair: \'discoverable as "velosterAA-test" for 3 minutes\'' % kind,
              r.logged('discoverable as "velosterAA-test" for 3 minutes'))
        mark = time.time()
        res = {'RequestConfirmation': r.agent('RequestConfirmation', dbus.ObjectPath(STRANGER), dbus.UInt32(4321))[0],
               'RequestPinCode': r.agent('RequestPinCode', dbus.ObjectPath(STRANGER)),
               'RequestPasskey': r.agent('RequestPasskey', dbus.ObjectPath(STRANGER))}
        res['RequestPinCode'] = (res['RequestPinCode'][0], str(res['RequestPinCode'][1]))
        res['RequestPasskey'] = (res['RequestPasskey'][0], int(res['RequestPasskey'][1] or 0))
        seen['agent'] = res
        trusted = [c[1:] for c in bz.since(mark, 'Set')]
        seen['trusted'] = trusted
        check('%s --pair: confirmation accepted and the device trusted; PIN 0000; passkey 0' % kind,
              res == {'RequestConfirmation': 'ok', 'RequestPinCode': ('ok', '0000'), 'RequestPasskey': ('ok', 0)}
              and ('Set', STRANGER, 'Trusted', 'Boolean', 1) in trusted, (res, trusted))
        check('%s --pair: passkey logged as 004321' % kind, r.logged('passkey 004321: accepted'))
    finally:
        code = r.stop()
    check('cpp --pair: SIGTERM: "aa: stopping", exit 0', code == 0 and r.logged('aa: stopping', 1), code)
    return seen


def scenario_hold(kind, binary, bz, address, tmp):
    """Pairing opened and closed from aaview's icon (a 3 s hold: "pair on" / "pair off" from aarecvd)."""
    print('\n== aarecvd --bt-only: pairing from the icon', flush=True)
    bz.mode = 'ok'
    start = time.time()
    r = Run(kind, binary, bz, address, tmp)
    try:
        check('hold: registers, not pairable, no pairing file', wait_for(lambda: bz.since(start, 'RequestDefaultAgent'), 15)
              and r.logged('listening to it on', 5) and r.pairing() is None)
        rej = r.agent('RequestConfirmation', dbus.ObjectPath(STRANGER), dbus.UInt32(111111))[0]
        check('hold: before it, pairing is refused', rej == 'org.bluez.Error.Rejected', rej)
        r.notify('session up')                          # it works during a session too
        mark = time.time()
        r.notify('pair on')
        ok = wait_for(lambda: ('Set', '/org/bluez/hci0', 'Discoverable', 'Boolean', 1)
                      in [c[1:] for c in bz.since(mark, 'Set')], 5)
        sets = [c[1:] for c in bz.since(mark, 'Set')]
        check('hold: "pair on" -> pairable, discoverable for 180 s', ok
              and sets[:3] == [('Set', '/org/bluez/hci0', 'Pairable', 'Boolean', 1),
                               ('Set', '/org/bluez/hci0', 'DiscoverableTimeout', 'UInt32', 180),
                               ('Set', '/org/bluez/hci0', 'Discoverable', 'Boolean', 1)], sets)
        p = r.pairing()
        check('hold: the pairing file says open for about 180 s, no code yet',
              p is not None and 170 < p[0] <= 181 and p[1] == 0, p)
        check('hold: logged', r.logged('for 3 minutes: pair from the phone\'s Bluetooth settings (the icon was held)'))
        res = r.agent('RequestConfirmation', dbus.ObjectPath(STRANGER), dbus.UInt32(123456))[0]
        p = r.pairing()
        check('hold: a phone pairing: accepted, its code in the file for aaview', res == 'ok' and p and p[1] == 123456,
              (res, p))
        rej = r.agent('RequestConfirmation', dbus.ObjectPath(SECOND), dbus.UInt32(654321))[0]
        p = r.pairing()
        check('hold: a second device is refused while the first is pairing',
              rej == 'org.bluez.Error.Rejected' and p and p[1] == 123456 and r.logged('held for another device', 2),
              (rej, p))
        mark = time.time()
        r.notify('pair off')
        ok = wait_for(lambda: r.pairing() is None, 5)
        sets = [c[1:] for c in bz.since(mark, 'Set')]
        check('hold again: "pair off" -> not pairable or discoverable, file gone', ok
              and ('Set', '/org/bluez/hci0', 'Pairable', 'Boolean', 0) in sets
              and ('Set', '/org/bluez/hci0', 'Discoverable', 'Boolean', 0) in sets, sets)
        rej = r.agent('RequestConfirmation', dbus.ObjectPath(STRANGER), dbus.UInt32(222222))[0]
        check('hold again: pairing refused once more', rej == 'org.bluez.Error.Rejected', rej)
        r.notify('pair on')
        wait_for(lambda: r.pairing() is not None, 5)
        r.agent('RequestConfirmation', dbus.ObjectPath(STRANGER), dbus.UInt32(333333))
        check('a phone paired: pairing closes by itself 20 s later',
              wait_for(lambda: r.pairing() is None, 25) and r.logged('a phone paired: not pairable any more', 2))
    finally:
        code = r.stop()
    check('hold: SIGTERM: exit 0, no pairing file left', code == 0 and r.pairing() is None, code)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    binary = os.path.realpath(args[0] if args else os.path.join(HERE, '..', 'out', 'aarecvd-amd64'))
    if os.path.exists('/sys/bus/serial/devices/serial0-0') or os.path.exists('/usr/local/lib/veloaa'):
        # It writes /etc/aarecv.conf and /etc/aarecv/loglevel.
        sys.exit('this looks like the Pi: the Bluetooth desk test runs only in the build image (build.sh bttest)')

    tmp = tempfile.mkdtemp(prefix='aarecvd-bt-')
    os.makedirs('/etc/aarecv', exist_ok=True)
    with open('/etc/aarecv.conf', 'w') as f:
        f.write(''.join('%s=%s\n' % kv for kv in AP.items()))
    with open('/etc/aarecv/loglevel', 'w') as f:
        f.write('debug\n')
    conf = os.path.join(tmp, 'bus.conf')
    with open(conf, 'w') as f:
        f.write('<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN" '
                '"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">\n<busconfig><type>session</type>'
                '<listen>unix:path=%s/bus</listen><auth>EXTERNAL</auth><policy context="default">'
                '<allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/>'
                '</policy></busconfig>\n' % tmp)
    daemon = subprocess.Popen(['dbus-daemon', '--config-file', conf, '--nofork', '--print-address'],
                              stdout=subprocess.PIPE, text=True)
    address = daemon.stdout.readline().strip()
    print('private bus: %s' % address, flush=True)

    dbus.mainloop.glib.threads_init()
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.bus.BusConnection(address)
    name = dbus.service.BusName('org.bluez', bus)   # kept, since the name goes when this object does
    bz = Bluez(bus)
    bz.name = name
    loop = GLib.MainLoop()
    threading.Thread(target=loop.run, daemon=True).start()

    try:
        cpp = scenario('cpp', binary, bz, address, tmp)
        cpp['pair'] = scenario_pair('cpp', binary, bz, address, tmp)
        scenario_hold('cpp', binary, bz, address, tmp)
        print('\n== aarecvd against the recorded reference', flush=True)
        ref = REFERENCE
        for key in ('registration', 'adapter_sets', 'agent', 'wifi_start', 'wifi_info', 'first_connects', 'nudge',
                    'inprogress', 'noreply_exit'):
            check('matches the reference: %s' % key, ref.get(key) == cpp.get(key), (ref.get(key), cpp.get(key)))
        for key in ('adapter_sets', 'agent', 'trusted'):
            check('matches the reference (--pair): %s' % key, ref['pair'].get(key) == cpp['pair'].get(key),
                  (ref['pair'].get(key), cpp['pair'].get(key)))
        print('\n  memory after start-up: aarecvd %d KB' % cpp['rss_kb'])
    finally:
        loop.quit()
        daemon.terminate()

    failed = [n for n, ok in RESULTS if not ok]
    print('\n%d checks, %d failed' % (len(RESULTS), len(failed)))
    print('BT SELFTEST %s' % ('PASSED' if not failed else 'FAILED'))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
