"""List and extract the contents of a Windows CE B000FF ROM image (NK.bin).

usage:
    python nkbin.py list NK.bin            # header, ROMHDR, modules and files
    python nkbin.py dump NK.bin OUTDIR     # write uncompressed FILES entries (e.g. ceconfig.h) to OUTDIR
    python nkbin.py flat NK.bin OUT.rom    # write the flat in-memory image (for XIP extractors)

Format notes (CE 6):
  "B000FF\\n", image start (u32), image length (u32), then records of
  (address u32, length u32, checksum u32, data). A record with address 0 ends the image.
  At image start + 0x40 sits "ECEC" followed by a pointer to ROMHDR. ROMHDR is followed by
  nummods TOCentry (32 bytes each) and numfiles FILESentry (28 bytes each).
Modules are stored as e32/o32 sections, not PE files; rebuilding PEs is a later step.
"""
import struct
import sys
from pathlib import Path

ROMHDR_FMT = '<' + 'I' * 17 + 'HH' + 'I' * 3
ROMHDR_FIELDS = ('dllfirst dlllast physfirst physlast nummods ulRAMStart ulRAMFree ulRAMEnd '
                 'ulCopyEntries ulCopyOffset ulProfileLen ulProfileOffset numfiles ulKernelFlags '
                 'ulFSRamPercent ulDrivglobStart ulDrivglobLen usCPUType usMiscFlags pExtensions '
                 'ulTrackingStart ulTrackingLen').split()
TOC_FMT = '<I8sIIIII'      # attrs, filetime, size, name ptr, e32 offset, o32 offset, load offset
FILES_FMT = '<I8sIIII'     # attrs, filetime, real size, compressed size, name ptr, load offset


class Image:
    def __init__(self, path: Path):
        raw = path.read_bytes()
        if raw[:7] != b'B000FF\n':
            raise SystemExit(f'{path}: not a B000FF image')
        self.start, self.length = struct.unpack_from('<II', raw, 7)
        self.mem = bytearray(self.length)
        off, self.records = 15, 0
        while off + 12 <= len(raw):
            addr, size, _chk = struct.unpack_from('<III', raw, off)
            off += 12
            if addr == 0:
                self.entry = size
                break
            rel = addr - self.start
            if 0 <= rel and rel + size <= self.length:
                self.mem[rel:rel + size] = raw[off:off + size]
            off += size
            self.records += 1

    def u32(self, addr):
        return struct.unpack_from('<I', self.mem, addr - self.start)[0]

    def cstr(self, addr):
        rel = addr - self.start
        if not 0 <= rel < self.length:
            return f'<bad ptr {addr:#x}>'
        end = self.mem.index(0, rel)
        return self.mem[rel:end].decode('latin-1')

    def bytes_at(self, addr, n):
        rel = addr - self.start
        return bytes(self.mem[rel:rel + n])


def parse(img: Image):
    sig = img.bytes_at(img.start + 0x40, 4)
    if sig != b'ECEC':
        raise SystemExit(f'no ECEC signature at image start + 0x40 (got {sig!r})')
    romhdr_addr = img.u32(img.start + 0x44)
    hdr = dict(zip(ROMHDR_FIELDS, struct.unpack_from(ROMHDR_FMT, img.mem, romhdr_addr - img.start)))
    toc = romhdr_addr + struct.calcsize(ROMHDR_FMT)
    mods = []
    for i in range(hdr['nummods']):
        attrs, _ft, size, name, e32, o32, load = struct.unpack_from(TOC_FMT, img.mem, toc - img.start + i * 32)
        mods.append((img.cstr(name), size, attrs))
    files_at = toc + hdr['nummods'] * 32
    files = []
    for i in range(hdr['numfiles']):
        attrs, _ft, real, comp, name, load = struct.unpack_from(FILES_FMT, img.mem, files_at - img.start + i * 28)
        files.append((img.cstr(name), real, comp, load, attrs))
    return romhdr_addr, hdr, mods, files


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd not in ('list', 'dump', 'flat') or len(sys.argv) < (3 if cmd == 'list' else 4):
        raise SystemExit(__doc__)
    img = Image(Path(sys.argv[2]))
    if sys.argv[1] == 'flat':
        Path(sys.argv[3]).write_bytes(img.mem)
        print(f'wrote {len(img.mem)} bytes, image base {img.start:#010x}')
        return
    romhdr_addr, hdr, mods, files = parse(img)
    if sys.argv[1] == 'list':
        print(f'image {img.start:#010x} + {img.length:#x}, {img.records} records, entry {img.entry:#010x}')
        print(f'ROMHDR at {romhdr_addr:#010x}: CPU type {hdr["usCPUType"]:#x}, '
              f'RAM {hdr["ulRAMStart"]:#x}-{hdr["ulRAMEnd"]:#x}, {hdr["nummods"]} modules, {hdr["numfiles"]} files')
        print('\n# modules (name, size)')
        for name, size, _a in sorted(mods, key=lambda m: m[0].lower()):
            print(f'{name:32} {size:9}')
        print('\n# files (name, real size, stored size)')
        for name, real, comp, _l, _a in sorted(files, key=lambda f: f[0].lower()):
            print(f'{name:32} {real:9} {comp:9}{"  compressed" if comp != real else ""}')
        return
    out = Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    for name, real, comp, load, _a in files:
        if comp == real:
            (out / name).write_bytes(img.bytes_at(load, real))
            print(f'wrote {name} ({real} bytes)')
        else:
            print(f'skipped {name}: compressed ({comp} -> {real}); decompressor not written yet')


if __name__ == '__main__':
    main()
