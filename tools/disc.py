#!/usr/bin/env python3
"""Disc image helpers for setting up the game from your own Kingpin CD rip (BIN/CUE).

  disc.py iso <sheet.cue> <out.iso>        data track (MODE1/MODE2 2352-byte sectors) -> 2048-byte ISO
  disc.py ls <image.iso> [dir]             list a directory of the ISO 9660 image
  disc.py get <image.iso> <path> <out>     copy one file out of the image
"""
import os
import re
import struct
import sys


def cue_data_track(cue):
    """(bin path, sector mode) of the cue sheet's first data track."""
    d = os.path.dirname(os.path.abspath(cue))
    current = None
    for line in open(cue, encoding='latin-1'):
        m = re.search(r'FILE\s+"(.+)"', line)
        if m:
            current = os.path.join(d, m.group(1))
        m = re.search(r'TRACK\s+\d+\s+(MODE[12])/2352', line)
        if m and current:
            return current, m.group(1)
    sys.exit('no MODE1/2352 or MODE2/2352 data track in ' + cue)


def to_iso(cue, out):
    path, mode = cue_data_track(cue)
    offset = 16 if mode == 'MODE1' else 24  # sync + header (+ subheader for MODE2 form 1)
    with open(path, 'rb') as f, open(out, 'wb') as o:
        while True:
            sector = f.read(2352)
            if len(sector) < 2352:
                break
            o.write(sector[offset:offset + 2048])
    print('wrote', out, os.path.getsize(out), 'bytes')


class Iso:
    def __init__(self, path):
        self.f = open(path, 'rb')
        pvd = self.read(16 * 2048, 2048)
        if pvd[1:6] != b'CD001':
            sys.exit(path + ' is not an ISO 9660 image')
        self.root = self.record(pvd, 156)

    def read(self, offset, n):
        self.f.seek(offset)
        return self.f.read(n)

    @staticmethod
    def record(buf, p):
        extent, size, flags, nlen = struct.unpack_from('<I', buf, p + 2)[0], struct.unpack_from('<I', buf, p + 10)[0], buf[p + 25], buf[p + 32]
        name = buf[p + 33:p + 33 + nlen].decode('latin-1').split(';')[0].rstrip('.')
        return {'name': name, 'offset': extent * 2048, 'size': size, 'dir': bool(flags & 2)}

    def list(self, d):
        data, out, p = self.read(d['offset'], d['size']), [], 0
        while p < len(data):
            n = data[p]
            if n == 0:
                p = (p // 2048 + 1) * 2048
                continue
            if not (data[p + 32] == 1 and data[p + 33] in (0, 1)):
                out.append(self.record(data, p))
            p += n
        return out

    def find(self, path):
        cur = self.root
        for part in [x for x in path.replace('\\', '/').split('/') if x]:
            match = [e for e in self.list(cur) if e['name'].lower() == part.lower()]
            if not match:
                sys.exit('not on the disc: ' + path)
            cur = match[0]
        return cur


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == 'iso' and len(sys.argv) == 4:
        to_iso(sys.argv[2], sys.argv[3])
    elif cmd == 'ls':
        iso = Iso(sys.argv[2])
        for e in iso.list(iso.find(sys.argv[3] if len(sys.argv) > 3 else '/')):
            print(('%-12s' % 'dir' if e['dir'] else '%12d' % e['size']), e['name'])
    elif cmd == 'get' and len(sys.argv) == 5:
        iso = Iso(sys.argv[2])
        e = iso.find(sys.argv[3])
        with open(sys.argv[4], 'wb') as o:
            left, off = e['size'], e['offset']
            while left:
                chunk = iso.read(off, min(left, 1 << 22))
                o.write(chunk)
                off += len(chunk)
                left -= len(chunk)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
