"""Optional dependencies: etcpak, Pillow, texture2ddecoder (independent decoder)."""
import importlib.util
import io
from pathlib import Path
import struct
import unittest

try:
    import etcpak
    from PIL import Image
    import texture2ddecoder
    HAVE_DEPS = True
except ImportError:
    HAVE_DEPS = False


@unittest.skipUnless(HAVE_DEPS, 'ETC2 test dependencies not installed')
class Etc2Test(unittest.TestCase):
    def test_mips_padding_and_channels(self):
        spec = importlib.util.spec_from_file_location(
            'make_etc2', Path(__file__).resolve().parents[1] / 'tools/make_etc2.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        source = io.BytesIO()
        Image.new('RGBA', (7, 5), (255, 0, 0, 255)).save(source, format='TGA')
        result = module.encode(source.getvalue())
        self.assertEqual(struct.unpack_from('<4sIII', result), (b'KPE2', 7, 5, 3))
        pos, w, h = 16, 7, 5
        for level in range(3):
            size, = struct.unpack_from('<I', result, pos)
            pos += 4
            self.assertEqual(size, ((w+3)//4)*((h+3)//4)*16)
            decoded = texture2ddecoder.decode_etc2a8(result[pos:pos+size], (w+3)&~3, (h+3)&~3)
            # Independent decoder returns BGRA. Red must stay red, including padded mips.
            self.assertGreater(decoded[2], 240)
            self.assertLess(decoded[0], 15)
            self.assertGreater(decoded[3], 240)
            pos += size
            w, h = max(1, w//2), max(1, h//2)
        self.assertEqual(pos, len(result))
