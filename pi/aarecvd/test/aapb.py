"""Protobuf and socket helpers for the desk tests, and the fixed numbers they check against."""
import struct

PORT = 5288                                                    # the Android Auto TCP port
AAW_UUID = '4de17a00-52cb-11e6-bdf4-0800200c9a66'              # AA wireless (RFCOMM)
HSP_HS_UUID = '00001108-0000-1000-8000-00805f9b34fb'           # headset
HSP_AG_UUID = '00001112-0000-1000-8000-00805f9b34fb'           # headset audio gateway (the phone's side)


def varint(n):
    n &= (1 << 64) - 1
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def pv(num, v):                      # varint field
    return varint(num << 3) + varint(v)


def pb(num, data):                   # length-delimited field (bytes, string, sub-message)
    if isinstance(data, str):
        data = data.encode()
    return varint((num << 3) | 2) + varint(len(data)) + data


def _rv(b, i):
    n = shift = 0
    while True:
        c = b[i]
        i += 1
        n |= (c & 0x7f) << shift
        shift += 7
        if not c & 0x80:
            return n, i


def parse(b):
    """Decodes a protobuf message to {field: [values]}: varints and fixed fields as int, length-delimited as bytes."""
    out, i = {}, 0
    while i < len(b):
        key, i = _rv(b, i)
        num, wt = key >> 3, key & 7
        if wt == 0:
            v, i = _rv(b, i)
        elif wt == 2:
            n, i = _rv(b, i)
            v, i = bytes(b[i:i + n]), i + n
        elif wt == 1:
            v, i = struct.unpack_from('<Q', b, i)[0], i + 8
        elif wt == 5:
            v, i = struct.unpack_from('<I', b, i)[0], i + 4
        else:
            break
        out.setdefault(num, []).append(v)
    return out


def recvn(s, n):
    b = bytearray()
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError('connection closed')
        b += c
    return bytes(b)
