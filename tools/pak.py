#!/usr/bin/env python3
"""List or extract Quake 2 / Kingpin PAK files.  usage: pak.py <pak> [name-substring [out-dir]]"""
import os
import struct
import sys

f = open(sys.argv[1], 'rb')
magic, off, size = struct.unpack('<4sII', f.read(12))
assert magic == b'PACK'
f.seek(off)
d = f.read(size)
for k in range(size // 64):
    name, o, n = struct.unpack_from('<56sII', d, 64 * k)
    name = name.split(b'\0')[0].decode()
    if len(sys.argv) > 2 and sys.argv[2] not in name:
        continue
    if len(sys.argv) > 3:
        p = os.path.join(sys.argv[3], name)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        f.seek(o)
        open(p, 'wb').write(f.read(n))
    else:
        print(n, name)
