"""No copyrighted fixtures required: python -m unittest discover -s tests."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location(
    'bundle_android_data', Path(__file__).resolve().parents[1] / 'tools/bundle_android_data.py')
bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundle)


class BundleTest(unittest.TestCase):
    def setUp(self):
        self.original = Path.cwd()
        self.tmp = tempfile.TemporaryDirectory()
        os.chdir(self.tmp.name)
        for name in ('game/kingpin.exe', 'game/ref_gl.dll', 'game/main/gamex86.dll',
                     'game/main/Pak0.pak', 'track1.iso'):
            path = Path(name)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'fixture')

    def tearDown(self):
        os.chdir(self.original)
        self.tmp.cleanup()

    def test_original_disc_case(self):
        output = Path('build/assets/data.zip')
        bundle.bundle(output)
        with zipfile.ZipFile(output) as archive:
            self.assertIn('main/Pak0.pak', archive.namelist())
            self.assertIsNone(archive.testzip())

    def test_missing_pak(self):
        Path('game/main/Pak0.pak').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing or empty'):
            bundle.bundle(Path('out/data.zip'))

    def test_empty_pak(self):
        Path('game/main/Pak0.pak').write_bytes(b'')
        with self.assertRaisesRegex(ValueError, 'Missing or empty'):
            bundle.bundle(Path('out/data.zip'))


if __name__ == '__main__':
    unittest.main()
