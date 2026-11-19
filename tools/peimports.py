"""Print the DLL imports of Windows CE PE files (MIPS or otherwise).

usage: python peimports.py FILE_OR_DIR [...]
For each PE file: machine type, subsystem, and every imported DLL with its functions
(by name, or #ordinal). CE binaries often import coredll by ordinal only.
"""
import struct
import sys
from pathlib import Path


def rva_to_off(sections, rva):
    for va, vsize, raw, rsize in sections:
        if va <= rva < va + max(vsize, rsize):
            return rva - va + raw
    return None


def imports(path: Path):
    b = path.read_bytes()
    if b[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    if b[pe:pe + 4] != b'PE\0\0':
        return None
    machine, nsec = struct.unpack_from('<HH', b, pe + 4)
    opt_size = struct.unpack_from('<H', b, pe + 20)[0]
    opt = pe + 24
    subsystem = struct.unpack_from('<H', b, opt + 68)[0]
    imp_rva = struct.unpack_from('<I', b, opt + 96 + 8)[0]     # data directory 1 (PE32)
    sec_at = opt + opt_size
    sections = []
    for i in range(nsec):
        va_size, va, rsize, raw = struct.unpack_from('<IIII', b, sec_at + i * 40 + 8)
        sections.append((va, va_size, raw, rsize))
    result = {'machine': machine, 'subsystem': subsystem, 'dlls': []}
    off = rva_to_off(sections, imp_rva) if imp_rva else None
    while off is not None:
        ilt, _ts, _fc, name_rva, iat = struct.unpack_from('<IIIII', b, off)
        if name_rva == 0:
            break
        noff = rva_to_off(sections, name_rva)
        dll = b[noff:b.index(b'\0', noff)].decode('latin-1')
        funcs = []
        t = rva_to_off(sections, ilt or iat)
        while t is not None:
            entry = struct.unpack_from('<I', b, t)[0]
            if entry == 0:
                break
            if entry & 0x80000000:
                funcs.append(f'#{entry & 0xffff}')
            else:
                h = rva_to_off(sections, entry)
                funcs.append(b[h + 2:b.index(b'\0', h + 2)].decode('latin-1'))
            t += 4
        result['dlls'].append((dll, funcs))
        off += 20
    return result


def main():
    paths = []
    for arg in sys.argv[1:]:
        p = Path(arg)
        paths += sorted(x for x in p.iterdir() if x.suffix.lower() in ('.exe', '.dll')) if p.is_dir() else [p]
    for p in paths:
        r = imports(p)
        if not r:
            continue
        print(f'== {p.name}  machine={r["machine"]:#x} subsystem={r["subsystem"]}')
        for dll, funcs in r['dlls']:
            print(f'   {dll}: {", ".join(funcs)}')


if __name__ == '__main__':
    main()
