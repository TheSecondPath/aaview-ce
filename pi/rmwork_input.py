#!/usr/bin/env python3
"""rmwork_input: Bluetooth keyboards, touchpads and gamepads on the Pi -> the head unit, via the mailbox LUN.

Reads every /dev/input/event* except HDMI ones, rescanning every 2 s so devices can come and go, and writes the
state into sector 0 of /run/veloaa/mailbox.img, which the head unit sees as raw store RMD0. vagent polls it and
replays it as Windows keyboard and mouse input. A game in front gets it as ordinary key messages plus mouse-delta messages.

Sector 0 layout, "RMI2" (little-endian):
   0  "RMI2"          magic
   4  u32 seq         incremented on every write
   8  s32 mouse_x     cumulative pointer motion; vagent works from the change between writes
  12  s32 mouse_y
  16  u8  buttons     bit 0 left, bit 1 right, bit 2 middle
  17  u8  presses[3]  cumulative press count per button (left, right, middle), mod 256: a tap that goes down
                      and up between two writes still shows up as a press
  20  u8  nkeys       number of Windows virtual-key codes held
  21  u8  vk[nkeys]   at most 64
 508  u32 seq         copy of seq. If the two differ, the reader caught a half-written sector and skips it
"""
import fcntl, glob, os, select, struct, time

MAILBOX = '/run/veloaa/mailbox.img'      # kept in RAM by veloaa-ramfiles.service, since it is rewritten 125 times a second
EV_FMT = 'llHHi'                     # struct input_event on 64-bit Linux
EV_SIZE = struct.calcsize(EV_FMT)
EV_KEY, EV_REL, EV_ABS = 1, 2, 3
EVIOCGRAB = 0x40044590               # _IOW('E', 0x90, int)
EVIOCGBIT_ABS = 0x80084523           # _IOC(_IOC_READ, 'E', 0x20 + EV_ABS, 8): the absolute axes a device has
REL_X, REL_Y = 0, 1
POINTER_SCALE = 2                    # touchpad counts -> pointer pixels / turning units
STICK_SPEED = 18                     # gamepad right stick at full deflection: pointer pixels per update
WRITE_HZ = 125

VK = dict(BACK=0x08, TAB=0x09, RETURN=0x0D, SHIFT=0x10, CONTROL=0x11, MENU=0x12, PAUSE=0x13, CAPITAL=0x14,
          ESCAPE=0x1B, SPACE=0x20, PRIOR=0x21, NEXT=0x22, END=0x23, HOME=0x24, LEFT=0x25, UP=0x26, RIGHT=0x27,
          DOWN=0x28, INSERT=0x2D, DELETE=0x2E, OEM_1=0xBA, OEM_PLUS=0xBB, OEM_COMMA=0xBC, OEM_MINUS=0xBD,
          OEM_PERIOD=0xBE, OEM_2=0xBF, OEM_3=0xC0, OEM_4=0xDB, OEM_5=0xDC, OEM_6=0xDD, OEM_7=0xDE)

# Linux key codes -> Windows virtual keys
KEYMAP = {1: VK['ESCAPE'], 14: VK['BACK'], 15: VK['TAB'], 28: VK['RETURN'], 96: VK['RETURN'], 57: VK['SPACE'],
          29: VK['CONTROL'], 97: VK['CONTROL'], 42: VK['SHIFT'], 54: VK['SHIFT'], 56: VK['MENU'], 100: VK['MENU'],
          58: VK['CAPITAL'], 119: VK['PAUSE'], 103: VK['UP'], 108: VK['DOWN'], 105: VK['LEFT'], 106: VK['RIGHT'],
          102: VK['HOME'], 107: VK['END'], 104: VK['PRIOR'], 109: VK['NEXT'], 110: VK['INSERT'], 111: VK['DELETE'],
          12: VK['OEM_MINUS'], 13: VK['OEM_PLUS'], 26: VK['OEM_4'], 27: VK['OEM_6'], 39: VK['OEM_1'],
          40: VK['OEM_7'], 41: VK['OEM_3'], 43: VK['OEM_5'], 51: VK['OEM_COMMA'], 52: VK['OEM_PERIOD'],
          53: VK['OEM_2']}
for code, ch in zip([16, 17, 18, 19, 20, 21, 22, 23, 24, 25], 'QWERTYUIOP'):
    KEYMAP[code] = ord(ch)
for code, ch in zip([30, 31, 32, 33, 34, 35, 36, 37, 38], 'ASDFGHJKL'):
    KEYMAP[code] = ord(ch)
for code, ch in zip([44, 45, 46, 47, 48, 49, 50], 'ZXCVBNM'):
    KEYMAP[code] = ord(ch)
for code, ch in zip(range(2, 12), '1234567890'):
    KEYMAP[code] = ord(ch)
for i in range(10):                  # F1-F10
    KEYMAP[59 + i] = 0x70 + i
KEYMAP[87], KEYMAP[88] = 0x7A, 0x7B  # F11, F12

BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 272, 273, 274

# Gamepads (8BitDo in D-input mode and most others): Linux BTN_* -> virtual keys (classic game keys)
PADMAP = {0x130: VK['CONTROL'],      # A / south: fire
          0x131: VK['SPACE'],        # B / east: use
          0x133: VK['SHIFT'],        # X / north: run
          0x134: VK['TAB'],          # Y / west: map
          0x136: VK['OEM_COMMA'], 0x137: VK['OEM_PERIOD'],   # L1 / R1: strafe
          0x138: VK['MENU'], 0x139: VK['CONTROL'],          # L2 / R2
          0x13a: VK['ESCAPE'], 0x13b: VK['RETURN'], 0x13c: VK['ESCAPE']}
ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y = 0, 1, 2, 3, 4, 5, 0x10, 0x11


class Dev:
    def __init__(self, path):
        self.path = path
        self.name = open('/sys/class/input/%s/device/name' % os.path.basename(path)).read().strip()
        self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        # Grab the device (EVIOCGRAB) so that only we read it. Otherwise the Pi's own console reads the keyboard too,
        # types into its login prompt, and turns Ctrl+Alt+Del, meant for the head unit's task manager, into a reboot.
        try:
            fcntl.ioctl(self.fd, EVIOCGRAB, 1)
        except OSError as e:
            print('input device: %s: could not take it for ourselves (%s): the Pi sees its keys too'
                  % (self.name, e.strerror), flush=True)
        self.held = set()
        self.stick = set()
        self.buttons = 0
        self.presses = [0, 0, 0]
        self.abs = {}
        self.absinfo = {}
        # pads without RX put the right stick on Z/RZ; on the others Z/RZ are the triggers
        bits = bytearray(8)
        try:
            fcntl.ioctl(self.fd, EVIOCGBIT_ABS, bits)
        except OSError:
            pass
        axes = int.from_bytes(bits, 'little')
        self.z_stick = bool(axes >> ABS_RZ & 1) and not axes >> ABS_RX & 1


def abs_range(path, code):
    buf = bytearray(24)              # EVIOCGABS(code): struct input_absinfo
    try:
        f = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        try:
            fcntl.ioctl(f, 0x80184540 + code, buf)
        finally:
            os.close(f)
        _, mn, mx = struct.unpack('iii', bytes(buf[:12]))
        return mn, mx
    except OSError:
        return -32768, 32767


def s32(x):
    return ((x + 2 ** 31) % 2 ** 32) - 2 ** 31


def main():
    devs = {}
    fd = os.open(MAILBOX, os.O_RDWR)
    seq, mx, my = 0, 0, 0
    last_scan, last_write, last_sig = 0.0, 0.0, None
    period = 1.0 / WRITE_HZ
    while True:
        now = time.time()
        if now - last_scan > 2:
            last_scan = now
            paths = set(glob.glob('/dev/input/event*'))
            for p in list(devs):
                if p not in paths:
                    os.close(devs.pop(p).fd)
            for p in paths - set(devs):
                try:
                    d = Dev(p)
                except OSError:
                    continue
                if 'hdmi' in d.name.lower():
                    os.close(d.fd)
                    continue
                devs[p] = d
                print('input device:', p, d.name, flush=True)
        ready = select.select([d.fd for d in devs.values()], [], [], period)[0] if devs else (time.sleep(period) or [])
        for d in list(devs.values()):
            if d.fd not in ready:
                continue
            try:
                data = os.read(d.fd, EV_SIZE * 64)
            except BlockingIOError:
                continue
            except OSError:
                os.close(d.fd)
                devs.pop(d.path, None)
                continue
            for i in range(0, len(data) - EV_SIZE + 1, EV_SIZE):
                _, _, typ, code, val = struct.unpack(EV_FMT, data[i:i + EV_SIZE])
                if typ == EV_KEY:
                    if code in (BTN_LEFT, BTN_RIGHT, BTN_MIDDLE):
                        bit = 1 << (code - BTN_LEFT)
                        if val and not d.buttons & bit:
                            d.presses[code - BTN_LEFT] += 1
                        d.buttons = (d.buttons | bit) if val else (d.buttons & ~bit)
                        continue
                    vk = PADMAP.get(code) if 0x130 <= code <= 0x13e else KEYMAP.get(code)
                    if vk is None:
                        continue
                    if val:              # 1 = press, 2 = autorepeat
                        d.held.add(vk)
                    else:
                        d.held.discard(vk)
                elif typ == EV_REL and code == REL_X:
                    mx += val * POINTER_SCALE
                elif typ == EV_REL and code == REL_Y:
                    my += val * POINTER_SCALE
                elif typ == EV_ABS:
                    if code not in d.absinfo:
                        d.absinfo[code] = abs_range(d.path, code)
                    d.abs[code] = val
        for d in devs.values():          # gamepad sticks / d-pad
            if not d.abs:
                continue
            def norm(c):
                lo, hi = d.absinfo.get(c, (-32768, 32767))
                v = d.abs.get(c, (lo + hi) // 2)
                return (2.0 * (v - lo) / max(1, hi - lo)) - 1.0
            st = set()
            lx, ly = norm(ABS_X), norm(ABS_Y)
            if ly < -0.35: st.add(VK['UP'])
            if ly > 0.35: st.add(VK['DOWN'])
            if lx < -0.35: st.add(VK['OEM_COMMA'])
            if lx > 0.35: st.add(VK['OEM_PERIOD'])
            hx, hy = d.abs.get(ABS_HAT0X, 0), d.abs.get(ABS_HAT0Y, 0)
            if hy < 0: st.add(VK['UP'])
            if hy > 0: st.add(VK['DOWN'])
            if hx < 0: st.add(VK['LEFT'])
            if hx > 0: st.add(VK['RIGHT'])
            d.stick = st
            rx = norm(ABS_RX) if ABS_RX in d.abs else (norm(ABS_Z) if d.z_stick else 0.0)
            ry = norm(ABS_RY) if ABS_RY in d.abs else (norm(ABS_RZ) if d.z_stick else 0.0)
            if abs(rx) > 0.15: mx += int(rx * STICK_SPEED)
            if abs(ry) > 0.15: my += int(ry * STICK_SPEED)
        held, buttons, presses = set(), 0, [0, 0, 0]
        for d in devs.values():
            held |= d.held | d.stick
            buttons |= d.buttons
            presses = [a + b for a, b in zip(presses, d.presses)]
        keys = sorted(held)[:64]
        sig = (tuple(keys), mx, my, buttons, tuple(presses))
        if sig != last_sig or now - last_write > 1.0:          # on change, and once a second as a heartbeat
            seq = (seq + 1) & 0xffffffff
            body = (b'RMI2' + struct.pack('<Iii', seq, s32(mx), s32(my))
                    + bytes([buttons] + [p & 0xff for p in presses] + [len(keys)]) + bytes(keys))
            os.pwrite(fd, body.ljust(508, b'\0') + struct.pack('<I', seq), 0)
            last_sig, last_write = sig, now


if __name__ == '__main__':
    main()
