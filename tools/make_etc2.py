#!/usr/bin/env python3
"""Encode user-owned TGA textures as ETC2 RGBA8 mip chains; no runtime encoder."""
import io
from pathlib import Path, PurePosixPath
import struct
import etcpak
from PIL import Image


def encode(data):
    image = Image.open(io.BytesIO(data)).convert('RGBA')
    w, h = image.size
    if w > 4096 or h > 4096:
        raise ValueError('Texture exceeds 4096 pixels')
    levels = []
    while True:
        # ETC2 blocks are 4x4 even for the final 1x1 mip; extend edge texels.
        pw, ph = (image.width + 3) & ~3, (image.height + 3) & ~3
        padded = Image.new('RGBA', (pw, ph))
        padded.paste(image, (0, 0))
        if pw > image.width:
            padded.paste(image.crop((image.width-1, 0, image.width, image.height)).resize((pw-image.width, image.height)), (image.width, 0))
        if ph > image.height:
            padded.paste(padded.crop((0, image.height-1, pw, image.height)).resize((pw, ph-image.height)), (0, image.height))
        compressed = etcpak.compress_etc2_rgba(padded.tobytes(), pw, ph)
        levels.append(struct.pack('<I', len(compressed)) + compressed)
        if image.size == (1, 1):
            break
        image = image.resize((max(1, image.width//2), max(1, image.height//2)), Image.Resampling.BOX)
    return struct.pack('<4sIII', b'KPE2', w, h, len(levels)) + b''.join(levels)


def sources(main):
    # Later PAKs override earlier ones, loose files override PAKs.
    found = {}
    for pak in sorted(main.glob('*'), key=lambda p: p.name.lower()):
        if pak.suffix.lower() != '.pak':
            continue
        with pak.open('rb') as f:
            magic, offset, size = struct.unpack('<4sII', f.read(12))
            if magic != b'PACK' or size % 64:
                raise ValueError('Invalid PAK directory')
            f.seek(offset)
            directory = f.read(size)
            for pos in range(0, size, 64):
                name, start, length = struct.unpack_from('<56sII', directory, pos)
                name = name.split(b'\0')[0].decode('ascii').lower()
                if name.endswith('.tga'):
                    f.seek(start)
                    found[name] = f.read(length)
    for p in main.rglob('*'):
        if p.suffix.lower() == '.tga' and p.is_file():
            found[p.relative_to(main).as_posix().lower()] = p.read_bytes()
    return found


def main():
    count = 0
    root = Path('game/main')
    for name, data in sources(root).items():
        path = PurePosixPath(name)
        # Never compress HUD, fonts, video or dynamic lightmaps.
        if path.is_absolute() or '..' in path.parts or path.parts[0] not in ('textures', 'models', 'sprites', 'env'):
            continue
        output = root / 'etc2' / path.with_suffix('.etc2')
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(encode(data))
        count += 1
    print(f'ETC2: encoded {count} TGA textures (other image formats retain original uploads)')


if __name__ == '__main__':
    main()
