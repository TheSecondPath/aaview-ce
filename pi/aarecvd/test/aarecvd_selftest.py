#!/usr/bin/env python3
"""Desk test for aarecvd against a fake phone (no Bluetooth, no head unit, no real phone).

It starts aarecvd on a free localhost port with a temporary stream image, settings file and notify socket, then
connects over TCP as the phone: version, a real TLS handshake (the fake phone is the TLS server, with a throwaway
self-signed certificate made here), service discovery (compared with recorded reference output),
video frames (some split into FIRST/MIDDLE/LAST frames), media and speech audio, audio/navigation focus, pings,
touches/keys/headlights written into aaview's status sector, the microphone, bye-bye, an icon tap with no session,
and a silent phone that must be dropped after 15 s.

  python3 aarecvd_selftest.py path/to/aarecvd [--keep]

Run it in the amd64 build image: pi/aarecvd/build.sh test
"""
import os, random, socket, ssl, struct, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)
from aapb import pb, pv, parse, recvn  # noqa: E402

# The recorded reference service discovery response (dpi 140). Phones accept exactly this, so aarecvd's must
# match it except for the two deliberate changes in compare_discovery().
REFERENCE_DISCOVERY = bytes.fromhex(
    '0a2a080122260803080408050806081308140815081608170854085508570858087e087f120608a00610e0030a0c08021208'
    '0a02080d0a02080a0a1508031a110803220b0801100118002000288c0128010a1208041a0e080110031a080880f702101018'
    '020a1108051a0d080110011a0708807d101018010a1108061a0d080110021a0708807d101018010a0f08072a0b0801120708'
    '807d101018011208726d776f726b70691a0856656c6f737465722204323031342a043030303130013a0b76656c6f73746572'
    '2d616142066161766965774a01315203302e3158016000')

RING_SECTOR, RING_BYTES = 16, (8192 - 16) * 512
RESULTS = []
PAIRFILE = None
HU_CRT = None


def check(name, ok, detail=''):
    RESULTS.append((name, bool(ok)))
    print('%-4s %s%s' % ('ok' if ok else 'FAIL', name, (': ' + detail) if detail else ''), flush=True)
    return ok


def first(d, n, default=None):
    v = d.get(n)
    return v[0] if v else default


# ---------------- the fake phone ----------------

class Phone:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=20)
        self.tls = self.inc = self.outb = None
        self.receiver_pings = 0
        self.backlog = []                       # messages read while waiting for another one

    def read_frame(self):
        ch, flags = recvn(self.s, 2)
        size = struct.unpack('>H', recvn(self.s, 2))[0]
        if flags & 3 == 1:
            recvn(self.s, 4)                    # total size of the whole message
        data = recvn(self.s, size)
        if flags & 8:
            self.inc.write(data)
            out = b''
            while True:
                try:
                    out += self.tls.read(65536)
                except ssl.SSLWantReadError:
                    break
            data = out
        return ch, flags, data

    def read_msg(self):
        parts = {}
        while True:
            ch, flags, data = self.read_frame()
            if flags & 3 == 3:
                msg = data
            elif flags & 3 == 1:
                parts[ch] = data
                continue
            else:
                parts[ch] = parts.get(ch, b'') + data
                if not flags & 2:
                    continue
                msg = parts.pop(ch)
            return ch, flags, struct.unpack('>H', msg[:2])[0], msg[2:]

    def send(self, ch, mid, body=b'', enc=True, control=False, split=0):
        payload = struct.pack('>H', mid) + body
        chunks = [payload] if not split else [payload[i:i + split] for i in range(0, len(payload), split)]
        for n, c in enumerate(chunks):
            fl = (1 if n == 0 else 0) | (2 if n == len(chunks) - 1 else 0) | (4 if control else 0)
            if enc:
                self.tls.write(c)
                c = self.outb.read()
                fl |= 8
            hdr = bytes([ch, fl]) + struct.pack('>H', len(c))
            if fl & 3 == 1:
                hdr += struct.pack('>I', len(payload))
            self.s.sendall(hdr + c)

    def handshake(self, crt, key, hu_ca=None):
        """Version exchange and TLS, with us as the TLS server as a phone is. Returns ((major, minor), the parsed
        AUTH_COMPLETE). With hu_ca, also require the head unit's certificate and accept only one signed by (here:
        equal to) that one."""
        ch, fl, mid, body = self.read_msg()
        version = struct.unpack('>HH', body) if (ch, mid) == (0, 1) else None
        self.send(0, 2, struct.pack('>HHH', 1, 6, 0), enc=False)
        sctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        sctx.load_cert_chain(crt, key)
        sctx.verify_mode = ssl.CERT_NONE
        if hu_ca:
            sctx.verify_mode = ssl.CERT_REQUIRED
            sctx.load_verify_locations(hu_ca)
        sctx.minimum_version = sctx.maximum_version = ssl.TLSVersion.TLSv1_2
        # The server's default cipher choice will do: aarecvd offers only AES-GCM, the one record layout aasdk decrypts.
        self.inc, self.outb = ssl.MemoryBIO(), ssl.MemoryBIO()
        self.tls = sctx.wrap_bio(self.inc, self.outb, server_side=True)
        while True:
            ch, fl, mid, body = self.read_msg()
            if mid == 4:
                self.peer = self.tls.getpeercert() if hu_ca else None
                return version, parse(body)
            assert mid == 3, mid
            self.inc.write(body)
            try:
                self.tls.do_handshake()
            except ssl.SSLWantReadError:
                pass
            o = self.outb.read()
            if o:
                self.send(0, 3, o, enc=False)

    def expect(self, ch, mid, timeout=5.0):
        """The next message on (ch, mid); the receiver's own pings are answered on the way, anything else is
        kept for a later expect()."""
        for i, (c, fl, m, body) in enumerate(self.backlog):
            if (c, m) == (ch, mid):
                del self.backlog[i]
                return fl, body
        end = time.time() + timeout
        self.s.settimeout(timeout)
        while time.time() < end:
            c, fl, m, body = self.read_msg()
            if c == 0 and m == 0xb:             # the receiver's ping: answer, as a phone does
                self.receiver_pings += 1
                self.send(0, 0xc, pv(1, first(parse(body), 1, 0)), enc=False)
                if (ch, mid) != (0, 0xb):
                    continue
            if (c, m) == (ch, mid):
                return fl, body
            self.backlog.append((c, fl, m, body))
        raise TimeoutError('no message 0x%04x on channel %d' % (mid, ch))


# ---------------- helpers on the stream image ----------------

def header(fd):
    b = os.pread(fd, 512, 0)
    magic = b[:4]
    seq, session, wr, ring_sector, ring_bytes, idr_pos, w, h, flags, mic_gen, utc = struct.unpack_from('<11I', b, 4)
    gmtoff = struct.unpack_from('<i', b, 48)[0]
    copy = struct.unpack_from('<I', b, 504)[0]
    return dict(magic=magic, seq=seq, session=session, wr=wr, ring_sector=ring_sector, ring_bytes=ring_bytes,
                idr_pos=idr_pos, w=w, h=h, flags=flags, mic_gen=mic_gen, utc=utc, gmtoff=gmtoff, copy=copy)


class Status:
    """aaview's status sector (sector 1), written the way aaview writes it."""

    def __init__(self, fd, session=77):
        self.fd, self.session, self.seq, self.events = fd, session, 0, []

    def write(self, new_events=(), flags=0):
        self.events += list(new_events)
        nev = len(self.events)
        self.seq += 1
        b = bytearray(512)
        struct.pack_into('<4s9I', b, 0, b'AVH1', self.seq, self.session, 0, 0, 0, 0, flags, 0, nev)
        for i in range(max(0, nev - 16), nev):
            x, y, action = self.events[i]
            struct.pack_into('<HHBBH', b, 40 + (i % 16) * 8, x, y, action, 0, 0)
        struct.pack_into('<I', b, 504, self.seq)
        os.pwrite(self.fd, bytes(b), 512)


def ring_records(fd):
    """Every record from the start of the ring up to wr: (type, flags, rec_no, payload)."""
    wr = header(fd)['wr']
    assert wr <= RING_BYTES, 'the test wrote more than one ring'
    data = os.pread(fd, wr, RING_SECTOR * 512)
    out, off = [], 0
    while off < wr:
        n, rtype, rflags, pts, rec_no = struct.unpack_from('<IHHII', data, off)
        out.append((rtype, rflags, rec_no, data[off + 16:off + 16 + n]))
        off += (16 + n + 15) & ~15
    return out


def mic_slot(fd, gen, seq, pcm):
    s = struct.pack('<4s3I', b'AVM1', gen, seq, len(pcm)) + pcm
    s = s.ljust(2044, b'\0') + struct.pack('<I', seq)
    os.pwrite(fd, s, 2 * 512 + (seq % 3) * 2048)


# ---------------- a synthetic H.264 stream ----------------

def nal(t, size, rnd, first_mb0=True):
    body = bytes([0x80 if first_mb0 else 0x00]) + bytes(rnd.randrange(1, 256) for _ in range(size - 1))
    return b'\0\0\0\1' + bytes([0x60 | t]) + body


def make_clip(rnd, count=60):
    sps = b'\0\0\0\1\x67\x42\xc0\x1f' + bytes(rnd.randrange(1, 256) for _ in range(8))
    pps = b'\0\0\0\1\x68' + bytes(rnd.randrange(1, 256) for _ in range(4))
    aus = []
    for n in range(count):
        size = rnd.choice([600, 2500, 7000, 12000, 21000, 40000]) if n % 7 else 9000
        if n == 0:
            aus.append(('config+idr', sps + pps, nal(5, 30000, rnd)))
        elif n == 30:
            aus.append(('own-config-idr', sps + pps, nal(5, size, rnd)))   # a frame carrying its own SPS/PPS
        else:
            aus.append(('p', b'', nal(1, size, rnd)))
    return aus


def leveled(config, level=12):
    b = bytearray(config)
    i = 0
    while True:
        i = b.find(b'\0\0\1', i)
        if i < 0 or i + 6 >= len(b):
            break
        if b[i + 3] & 0x1f == 7:
            b[i + 6] = level
        i += 3
    return bytes(b)


# ---------------- service discovery, compared with the reference ----------------

SUB = {'sd': {1: 'service'}, 'service': {2: 'sensor', 3: 'sink', 4: 'input', 5: 'source'},
       'sensor': {1: 'msg'}, 'sink': {3: 'msg', 4: 'msg'}, 'input': {2: 'msg'}, 'source': {2: 'msg'}, 'msg': {}}
PACKED = {'input': (1,)}


def unpack_varints(b):
    out, i = [], 0
    while i < len(b):
        n = shift = 0
        while True:
            c = b[i]
            i += 1
            n |= (c & 0x7f) << shift
            shift += 7
            if not c & 0x80:
                break
        out.append(n)
    return out


def canon(b, kind):
    """A comparable form: {field: [values]} with sub-messages decoded and packed/unpacked repeated ints alike."""
    d = parse(b)
    out = {}
    for f, vals in sorted(d.items()):
        if f in SUB.get(kind, {}):
            out[f] = [canon(v, SUB[kind][f]) for v in vals]
        elif f in PACKED.get(kind, ()):
            flat = []
            for v in vals:
                flat += unpack_varints(v) if isinstance(v, bytes) else [v]
            out[f] = flat
        else:
            out[f] = vals
    return out


def compare_discovery(got_body):
    ref = canon(REFERENCE_DISCOVERY, 'sd')
    got = canon(got_body, 'sd')
    # Two deliberate differences: the reference's 1s read as 60 fps and right-hand drive on the phone, while aarecvd
    # asks for 30 fps (2) and left-hand drive (0). The rest must match byte for byte.
    ref[6] = [0]
    for c in ref[1]:
        for sink in c.get(3, []):
            for vcfg in sink.get(4, []):
                vcfg[2] = [2]
    top_ok = {k: v for k, v in ref.items() if k != 1} == {k: v for k, v in got.items() if k != 1}
    strip = lambda chans: [{k: v for k, v in c.items() if k != 1} for c in chans]
    chans_ok = strip(ref[1]) == strip(got[1])
    ids = [(first(r, 1), first(g, 1)) for r, g in zip(ref[1], got[1])]
    return top_ok, chans_ok, ids, ref, got


# ---------------- the test ----------------

def start_aarecvd(binary, tmp, name, extra=()):
    """aarecvd on a free localhost port with its own stream image, log and notify socket; debug log level."""
    img = os.path.join(tmp, name + '.img')
    settings = os.path.join(tmp, 'veloaa.txt')
    with open(settings, 'w') as f:
        f.write('# test\nlog_level = debug\nset_clock=off\n')
    sockpath = os.path.join(tmp, name + '.sock')
    global PAIRFILE
    PAIRFILE = os.path.join(tmp, name + '.pairing')
    notes = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    notes.bind(sockpath)
    notes.settimeout(0.2)
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    logpath = os.path.join(tmp, name + '.log')
    env = dict(os.environ)
    env.pop('JOURNAL_STREAM', None)
    proc = subprocess.Popen([binary, '--port', str(port), '--any-client', '--stream', img,
                             '--notify-socket', sockpath, '--settings', settings,
                             '--loglevel-file', os.path.join(tmp, 'none'),
                             '--keys-conf', os.path.join(tmp, 'none'), '--ap-conf', os.path.join(tmp, 'none'),
                             '--pairing-file', PAIRFILE, '--cert', os.path.join(tmp, 'none.crt'),
                             '--key', os.path.join(tmp, 'none.key')]
                            + list(extra), stdout=open(logpath, 'w'), stderr=subprocess.STDOUT, env=env)
    for _ in range(100):
        if os.path.exists(img) and os.path.getsize(img) == 8192 * 512:
            try:
                socket.create_connection(('127.0.0.1', port), timeout=0.2).close()
                break
            except OSError:
                pass
        time.sleep(0.05)
    return proc, port, img, notes, logpath


def stop(proc):
    proc.terminate()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', 'out', 'aarecvd-amd64')
    keep = '--keep' in sys.argv
    tmp = tempfile.mkdtemp(prefix='aarecvd-test-')
    crt, key = os.path.join(tmp, 't.crt'), os.path.join(tmp, 't.key')
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', key, '-out', crt,
                    '-subj', '/CN=fakephone', '-days', '1'], check=True, capture_output=True)
    global HU_CRT
    HU_CRT, hu_key = os.path.join(tmp, 'hu.crt'), os.path.join(tmp, 'hu.key')
    subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', hu_key, '-out', HU_CRT,
                    '-subj', '/CN=veloaa-test-hu', '-days', '1'], check=True, capture_output=True)
    logs = []
    # a certificate with somebody else's key: aasdk's built-in pair instead, said in the log
    proc, port, img, notes, logpath = start_aarecvd(binary, tmp, 'mismatch', ('--cert', HU_CRT, '--key', key))
    stop(proc)
    log = open(logpath).read()
    check('--cert / --key that do not belong together: the built-in certificate, with a warning',
          'do not belong together: using aasdk\'s built-in certificate' in log)
    logs.append(logpath)
    for name, extra, fn in (('main', (), run),
                            ('status', ('--status-services', '--cert', HU_CRT, '--key', hu_key), run_status)):
        proc, port, img, notes, logpath = start_aarecvd(binary, tmp, name, extra)
        logs.append(logpath)
        try:
            fn(port, img, crt, key, notes, logpath)
        except Exception as ex:
            import traceback
            traceback.print_exc()
            check('%s test ran to the end' % name, False, '%s: %s' % (type(ex).__name__, ex))
        finally:
            stop(proc)
    log = ''.join(open(p).read() for p in logs)
    check('no certificate or key in the logs', 'BEGIN' not in log and 'PRIVATE' not in log)
    failed = [n for n, ok in RESULTS if not ok]
    print('\n--- aarecvd log of the main test (last 40 lines) ---')
    print('\n'.join(open(logs[0]).read().splitlines()[-40:]))
    print('\n%d checks, %d failed%s' % (len(RESULTS), len(failed), (': ' + ', '.join(failed)) if failed else ''))
    print('SELFTEST PASSED' if not failed else 'SELFTEST FAILED')
    if keep:
        print('kept', tmp)
    sys.exit(0 if not failed else 1)


def notes_read(notes, want, timeout=2.0):
    end = time.time() + timeout
    got = []
    while time.time() < end:
        try:
            m = notes.recv(256).decode()
        except socket.timeout:
            continue
        got.append(m)
        if m == want:
            return True, got
    return False, got


def drain(notes):
    while True:
        try:
            notes.recv(256)
        except socket.timeout:
            return


def run(port, img, crt, key, notes, logpath):
    rnd = random.Random(5288)
    fd = os.open(img, os.O_RDWR)
    h0 = header(fd)
    check('stream image header', h0['magic'] == b'AVS1' and h0['ring_sector'] == 16 and
          h0['ring_bytes'] == RING_BYTES and (h0['w'], h0['h']) == (800, 480) and h0['copy'] == h0['seq'],
          'seq %d flags 0x%x' % (h0['seq'], h0['flags']))
    raw = os.pread(fd, 512, 0)
    check('header: nothing in the unused bytes (52 to 503 and the last 4)',
          raw[52:504] == bytes(452) and raw[508:] == bytes(4))
    time.sleep(2.3)
    h1 = header(fd)
    check('header heartbeat every 2 s, log level bits = debug (0), clock fields 0 with set_clock=off',
          h1['seq'] > h0['seq'] and (h1['flags'] >> 16) & 3 == 0 and h1['utc'] == 0 and h1['gmtoff'] == 0,
          'seq %d -> %d, flags 0x%x' % (h0['seq'], h1['seq'], h1['flags']))
    drain(notes)

    # ---- version, TLS ----
    ph = Phone(port)
    version, auth = ph.handshake(crt, key)
    time.sleep(0.2)
    log = open(logpath).read()
    check("no certificate files: aasdk's built-in one, said so (with its name and dates)",
          "aasdk's built-in head-unit certificate (no " in log and "aasdk's built-in certificate: " in log)
    check('version request 1.6', version == (1, 6), repr(version))
    check('TLS handshake and AUTH_COMPLETE', ph.tls.version() == 'TLSv1.2' and first(auth, 1, 0) == 0,
          '%s %s' % (ph.tls.version(), ph.tls.cipher()[0]))
    ok, got = notes_read(notes, 'session up')
    check('notify socket: "session up"', ok, repr(got))
    check('header flag: phone connected', header(fd)['flags'] & 4)

    # ---- service discovery ----
    ph.send(0, 5, pb(4, 'Fake Phone') + pb(5, 'Test'))
    fl, body = ph.expect(0, 6)
    top_ok, chans_ok, ids, ref, got = compare_discovery(body)
    check('service discovery: head-unit fields as the reference', top_ok)
    vcfgs = [v for c in got[1] for s in c.get(3, []) for v in s.get(4, [])]
    check('service discovery: left-hand drive (driver_position 0)', got.get(6) == [0], repr(got.get(6)))
    check('service discovery: 30 fps asked (VIDEO_FPS_30 = 2)', bool(vcfgs) and all(v.get(2) == [2] for v in vcfgs),
          repr([v.get(2) for v in vcfgs]))
    check('service discovery: channels as the reference (content and order)', chans_ok,
          'channel ids reference -> aarecvd: %s' % ', '.join('%s->%s' % i for i in ids))
    if not (top_ok and chans_ok):
        print('  reference:', ref)
        print('  aarecvd:  ', got)
    chan = {}
    for c in got[1]:
        kind = next(k for k in (2, 3, 4, 5) if k in c)
        chan.setdefault(kind, []).append(first(c, 1))
    VIDEO, MEDIA, SPEECH, SYSTEM = chan[3]
    INPUT, SENSOR, MIC = chan[4][0], chan[2][0], chan[5][0]
    check('channel numbers: video 3, audio 4/5/6, microphone 7 (as the reference)',
          (VIDEO, MEDIA, SPEECH, SYSTEM, MIC) == (3, 4, 5, 6, 7), 'input %d, sensor %d' % (INPUT, SENSOR))

    # ---- channel opens ----
    opened = True
    for c in (VIDEO, MEDIA, SPEECH, SYSTEM, INPUT, SENSOR, MIC):
        ph.send(c, 7, pv(1, 0) + pv(2, c), control=True)
        fl, body = ph.expect(c, 8)
        opened = opened and fl & 4 and first(parse(body), 1) == 0
    check('channel open requests answered (control flag, status 0)', opened)

    # ---- focus, ping ----
    ph.send(0, 0x12, pv(1, 2))
    fl, body = ph.expect(0, 0x13)
    a1 = first(parse(body), 1)
    ph.send(0, 0x12, pv(1, 4))
    fl, body = ph.expect(0, 0x13)
    a2 = first(parse(body), 1)
    ph.send(0, 0x12, pv(1, 1))
    fl, body = ph.expect(0, 0x13)
    a3 = first(parse(body), 1)
    check('audio focus: gain transient -> GAIN, release -> LOSS, gain -> GAIN', (a1, a2, a3) == (1, 3, 1),
          '%s %s %s' % (a1, a2, a3))
    ph.send(0, 0xd, pv(1, 2))
    fl, body = ph.expect(0, 0xe)
    check('navigation focus -> projected (2)', first(parse(body), 1) == 2)
    ph.send(0, 0xb, pv(1, 12345))
    fl, body = ph.expect(0, 0xc)
    check('the phone\'s ping answered with its timestamp', first(parse(body), 1) == 12345)

    # ---- video ----
    ph.send(VIDEO, 0x8000, pv(1, 3))
    fl, body = ph.expect(VIDEO, 0x8003)
    cfg = parse(body)
    fl, body = ph.expect(VIDEO, 0x8008)
    foc = parse(body)
    check('video setup: ready, max_unacked 8, then focus projected',
          (first(cfg, 1), first(cfg, 2), first(foc, 1), first(foc, 2, 0)) == (2, 8, 1, 0), '%s %s' % (cfg, foc))
    ph.send(VIDEO, 0x8001, pv(1, 7) + pv(2, 0))
    aus = make_clip(rnd)
    expected = []
    config = b''
    acks = 0
    bad_acks = []
    for n, (kind, cfg_bytes, frame) in enumerate(aus):
        if n == 0:
            ph.send(VIDEO, 1, cfg_bytes)        # codec config alone, like a phone
            fl, body = ph.expect(VIDEO, 0x8004)
            config = leveled(cfg_bytes)
            payload = frame
        else:
            payload = cfg_bytes + frame
            if cfg_bytes:
                config = leveled(cfg_bytes)
        idr = frame[4] & 0x1f == 5
        expected.append((1 if idr else 0, (config + frame) if idr else frame))
        ph.send(VIDEO, 0, struct.pack('>Q', n * 33333) + payload, split=4096 if len(payload) > 8000 else 0)
        fl, body = ph.expect(VIDEO, 0x8004)
        p = parse(body)
        if (first(p, 1), first(p, 2)) != (7, 1):
            bad_acks.append(p)
        acks += 1
    check('every frame acked with session 7', acks == len(aus) and not bad_acks, '%d acks' % acks)
    check('header flag: projecting', header(fd)['flags'] & 2)

    # ---- audio ----
    flags_before = header(fd)['flags']
    pcms = []
    for c in (MEDIA, SPEECH):
        ph.send(c, 0x8000, pv(1, 1))
        fl, body = ph.expect(c, 0x8003)
        cfg = parse(body)
        check('audio ch %d setup: ready, max_unacked 16' % c, (first(cfg, 1), first(cfg, 2)) == (2, 16), str(cfg))
    ph.send(MEDIA, 0x8001, pv(1, 3) + pv(2, 0))
    time.sleep(0.2)
    check('media start counted in header flags bits 8..15',
          (header(fd)['flags'] >> 8) & 0xff == ((flags_before >> 8) + 1) & 0xff)
    media_ok = True
    for n in range(5):
        pcm = bytes(rnd.randrange(256) for _ in range(4096))
        pcms.append((0, pcm))
        ph.send(MEDIA, 0, struct.pack('>Q', n) + pcm)
        fl, body = ph.expect(MEDIA, 0x8004)
        media_ok = media_ok and (first(parse(body), 1), first(parse(body), 2)) == (3, 1)
    check('media audio acked with session 3', media_ok)
    ph.send(SPEECH, 0x8001, pv(1, 4) + pv(2, 0))
    for n in range(3):
        samples = [rnd.randrange(-2000, 2000) for _ in range(800)]
        if n == 1:
            samples[100] = -12345
        pcm = struct.pack('<800h', *samples)
        pcms.append((1, pcm))
        ph.send(SPEECH, 0, struct.pack('>Q', n) + pcm)
        ph.expect(SPEECH, 0x8004)
    ph.send(SPEECH, 0x8002, b'')
    ph.send(MEDIA, 0x8002, b'')
    time.sleep(0.3)

    recs = ring_records(fd)
    video = [(fl_, p) for t, fl_, _, p in recs if t == 1]
    audio = [(fl_, p) for t, fl_, _, p in recs if t == 2]
    rec_nos = [r[2] for r in recs]
    check('every video frame in the ring, byte for byte (IDRs with SPS/PPS in front, level_idc 12; '
          'frames over 8000 bytes were sent in 4096-byte fragments)',
          any(len(f) > 8000 for _, _, f in aus) and video == expected,
          '%d of %d records match' % (sum(a == b for a, b in zip(video, expected)), len(expected)))
    check('audio records with stream ids (media 0, speech 1)', audio == pcms,
          '%d of %d' % (sum(a == b for a, b in zip(audio, pcms)), len(pcms)))
    pad_ok, data, off = True, os.pread(fd, header(fd)['wr'], RING_SECTOR * 512), 0
    while off < len(data):
        n = struct.unpack_from('<I', data, off)[0]
        total = (16 + n + 15) & ~15
        pad_ok = pad_ok and data[off + 16 + n:off + total] == bytes(total - 16 - n)
        off += total
    check('records 16-byte aligned with zero padding', pad_ok and off == len(data))
    check('record numbers consecutive', rec_nos == list(range(rec_nos[0], rec_nos[0] + len(rec_nos))))
    h = header(fd)
    idr_off = h['idr_pos']
    rec = os.pread(fd, 16, RING_SECTOR * 512 + idr_off)
    check('header idr_pos points at the last IDR record', struct.unpack_from('<IHH', rec)[1:] == (1, 1))

    # ---- sensors, touches, keys, headlights ----
    st = Status(fd)
    st.write()
    time.sleep(0.1)
    ph.send(SENSOR, 0x8001, pv(1, 13) + pv(2, 0))
    fl, body = ph.expect(SENSOR, 0x8002)
    fl, body = ph.expect(SENSOR, 0x8003)
    drive = parse(body)
    check('driving status: unrestricted', 13 in drive and first(parse(first(drive, 13)), 1, 0) == 0, str(drive))
    ph.send(SENSOR, 0x8001, pv(1, 10) + pv(2, 0))
    ph.expect(SENSOR, 0x8002)
    fl, body = ph.expect(SENSOR, 0x8003)
    n0 = parse(first(parse(body), 10, b''))
    st.write([(100, 200, 0), (110, 210, 2), (900, 600, 1), (0x0D, 0x1c, 3), (0x0D, 0x1c, 4), (301, 0, 5),
              (0x77, 0, 8)])
    reports = []
    for _ in range(7):
        fl, body = ph.expect(INPUT, 0x8001)
        reports.append(parse(body))
    fl, body = ph.expect(SENSOR, 0x8003)
    n1 = parse(first(parse(body), 10, b''))
    check('night mode: off at the start, on after the headlights came on',
          first(n0, 1, 0) == 0 and first(n1, 1) == 1, '%s -> %s' % (n0, n1))
    touches = []
    keys = []
    for r in reports:
        if 3 in r:
            t = parse(first(r, 3))
            ptr = parse(first(t, 1))
            touches.append((first(ptr, 1), first(ptr, 2), first(t, 3, 0)))
        if 4 in r:
            k = parse(first(parse(first(r, 4)), 1))
            keys.append((first(k, 1), first(k, 2)))
    check('touches from the status sector reach the phone (clamped to 799x479)',
          touches == [(100, 200, 0), (110, 210, 2), (799, 479, 1)], str(touches))
    check('keys: Enter down/up -> 0x17; steering seek up -> NEXT press+release',
          keys == [(0x17, 1), (0x17, 0), (0x57, 1), (0x57, 0)], str(keys))
    # 20 events in one status write: the sector holds the last 16, and those 16 still reach the phone
    burst = [(10 + i, 20, 2) for i in range(19)] + [(29, 20, 1)]
    st.write(burst)
    xs = []
    for _ in range(16):
        fl, body = ph.expect(INPUT, 0x8001)
        t = parse(first(parse(body), 3))
        xs.append((first(parse(first(t, 1)), 1), first(t, 3, 0)))
    check('a burst of 20 events: the newest 16 reach the phone, the lift last',
          xs == [(14 + i, 2) for i in range(15)] + [(29, 1)], str(xs))

    # ---- the icon held 3 s: Bluetooth pairing ----
    drain(notes)
    st.write([(1, 0, 9)])
    ok, got = notes_read(notes, 'pair on')
    st.write([(0, 0, 9)])
    ok2, got2 = notes_read(notes, 'pair off')
    check('icon held (event 9): "pair on" / "pair off" to aarecvd --bt-only, during a session too', ok and ok2,
          str(got + got2))
    pairing = lambda: (header(fd)['flags'] & 16, struct.unpack_from('<I', os.pread(fd, 512, 0), 52)[0])
    with open(PAIRFILE + '.tmp', 'w') as f:
        f.write('until %d\npasskey 0\n' % (time.monotonic() + 100))
    os.rename(PAIRFILE + '.tmp', PAIRFILE)
    time.sleep(0.6)
    open_nokey = pairing()
    with open(PAIRFILE + '.tmp', 'w') as f:
        f.write('until %d\npasskey 123456\n' % (time.monotonic() + 100))
    os.rename(PAIRFILE + '.tmp', PAIRFILE)
    time.sleep(0.6)
    open_key = pairing()
    os.unlink(PAIRFILE)
    time.sleep(0.6)
    closed = pairing()
    with open(PAIRFILE, 'w') as f:                       # left by a --bt-only that died: its time has passed
        f.write('until %d\npasskey 654321\n' % (time.monotonic() - 5))
    time.sleep(0.6)
    stale = pairing()
    os.unlink(PAIRFILE)
    check('pairing file -> header: bit 4 and the code at byte 52; gone or stale -> off',
          open_nokey == (16, 0) and open_key == (16, 123456) and closed == (0, 0) and stale == (0, 0),
          str((open_nokey, open_key, closed, stale)))

    # ---- microphone ----
    flags = lambda: header(fd)['flags']
    mic_slot(fd, header(fd)['mic_gen'] + 1, 9, b'\x55' * 2000)    # left over from "before": must be wiped
    ph.send(MIC, 0x8000, pv(1, 1))
    fl, body = ph.expect(MIC, 0x8003)
    check('microphone setup answered (0x8003); the header flag stays clear until the phone opens the microphone',
          not flags() & 8)
    ph.send(MIC, 0x8005, pv(1, 1) + pv(2, 0) + pv(3, 0) + pv(4, 8))
    fl, body = ph.expect(MIC, 0x8006)
    gen = header(fd)['mic_gen']
    check('microphone open answered with 0x8006 and header flag bit 3 set', flags() & 8)
    chunks = [bytes([n]) * 2000 for n in (1, 2, 3, 4, 5)]
    mic_slot(fd, gen - 1, 7, b'\x66' * 2000)                     # another request's chunk: ignored
    got = []
    for seq, pcm in enumerate(chunks, 1):
        mic_slot(fd, gen, seq, pcm)
        fl, body = ph.expect(MIC, 0)
        got.append(body[8:])
        ph.send(MIC, 0x8004, pv(1, 0) + pv(2, 1))               # the phone's ack: taken silently
    ph.send(MIC, 0x8005, pv(1, 0))
    fl, body = ph.expect(MIC, 0x8006)
    time.sleep(0.1)
    check('microphone: chunks in order, stale ones left out, flag cleared on close',
          got == chunks and not flags() & 8, '%d of %d' % (sum(a == b for a, b in zip(got, chunks)), len(chunks)))

    # ---- our pings (every 5 s) ----
    try:
        fl, body = ph.expect(0, 0xb, timeout=7)
        ph.send(0, 0xc, pv(1, first(parse(body), 1, 0)), enc=False)
        ph.receiver_pings += 1
    except TimeoutError:
        pass
    time.sleep(0.3)

    # ---- bye-bye ----
    ph.send(0, 0xf, pv(1, 1))
    fl, body = ph.expect(0, 0x10)
    ph.s.settimeout(5)
    closed = False
    try:
        while True:
            if not ph.s.recv(4096):
                closed = True
                break
    except (ConnectionResetError, TimeoutError, socket.timeout):
        closed = isinstance(sys.exc_info()[1], ConnectionResetError)
    check('bye-bye answered, then the connection closed', closed)
    ok, got = notes_read(notes, 'session ended')
    check('notify socket: "session ended"', ok, repr(got))
    time.sleep(0.2)
    check('header flags phone and projecting cleared', not header(fd)['flags'] & 6, hex(header(fd)['flags']))
    check('our pings reached the phone', ph.receiver_pings > 0, '%d pings' % ph.receiver_pings)

    # ---- icon tap with no session ----
    drain(notes)
    st.write([(1, 0, 7)])
    ok, got = notes_read(notes, 'icon 1')
    check('icon tap with no session -> "icon 1" to aarecvd --bt-only', ok, repr(got))

    # ---- a silent phone ----
    sp = Phone(port)
    ch, fl, mid, body = sp.read_msg()
    t0 = time.time()
    sp.s.settimeout(30)
    try:
        while sp.s.recv(4096):
            pass
    except ConnectionResetError:
        pass
    took = time.time() - t0
    check('a silent phone is dropped after 15 s', 14.0 <= took <= 17.5, '%.1f s' % took)

    log = open(logpath).read()
    for line in ('aa: audio focus request gain transient -> gain', 'aa: audio focus request release -> loss',
                 'aa: phone "Fake Phone" / "Test"', 'aa: video start, session 7 config 0',
                 'aa: audio ch 5: that sound was 150 ms, loudest sample 12345 of 32767',
                 'aa: microphone closed by the phone', 'aa: session ended: the phone shut the session down',
                 'aa: session ended: timed out: nothing from the phone', 'log: level debug (from veloaa.txt)',
                 'aa: ping answered in ',
                 'aa: SPS profile 66 level 31 -> 12'):
        check('log line: ' + line, line in log)
    os.close(fd)


def run_status(port, img, crt, key, notes, logpath):
    """--status-services: the three log-only services are offered and log what the phone sends. This run's head-unit
    certificate comes from files (--cert / --key), and the fake phone accepts only that one."""
    ph = Phone(port)
    ph.handshake(crt, key, hu_ca=HU_CRT)
    subject = dict(x[0] for x in (ph.peer or {}).get('subject', ()))
    check('--cert / --key: the head unit presents the certificate from the files', subject.get('commonName') ==
          'veloaa-test-hu', str(ph.peer))
    log = open(logpath).read()
    check('--cert / --key: logged by name and dates only',
          'head-unit certificate from %s' % HU_CRT in log and 'CN=veloaa-test-hu, valid ' in log)
    ph.send(0, 5, pb(4, 'Fake Phone') + pb(5, 'Test'))
    fl, body = ph.expect(0, 6)
    got = parse(body)
    kinds = {}
    for c in got[1]:
        p = parse(c)
        kinds.update({k: first(p, 1) for k in p if k != 1})
    check('--status-services: navigation (8), media playback (9), phone status (10) offered',
          all(k in kinds for k in (8, 9, 10)), str(kinds))
    check('no Bluetooth service (6) offered: the head unit has its own hands-free',
          6 not in kinds, str(kinds))
    nav, media, phone = kinds.get(8, 12), kinds.get(9, 13), kinds.get(10, 14)
    for c in (nav, media, phone):
        ph.send(c, 7, pv(1, 0) + pv(2, c), control=True)
        ph.expect(c, 8)
    ph.send(phone, 0x8001, pb(1, pv(1, 4) + pv(2, 0) + pb(3, '+15551234') + pb(4, 'Mom')) + pv(2, 3))
    ph.send(media, 0x8003, pb(1, 'Song') + pb(2, 'Artist') + pb(4, b'\x89PNG' * 100) + pv(6, 200))
    ph.send(media, 0x8001, pv(1, 2) + pb(2, 'Spotify') + pv(3, 12))
    ph.send(nav, 0x8003, pv(1, 1))
    time.sleep(0.5)
    log = open(logpath).read()
    check('phone status logged', 'aa: phone status {' in log and 'phone_state: INCOMING' in log and 'Mom' in log)
    check('media metadata logged (album art as its size)',
          'aa: media playing {song: "Song"' in log and '<400 bytes>' in log)
    check('media playback logged', 'aa: media playback {state: PLAYING media_source: "Spotify"' in log)
    check('navigation status logged', 'aa: navigation status {status: ACTIVE}' in log)


if __name__ == '__main__':
    main()
