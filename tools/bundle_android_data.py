#!/usr/bin/env python3
"""Package locally installed, user-owned game data for a personal Android APK."""
from pathlib import Path
import zipfile


def bundle(output):
    required = [Path('game') / p for p in (
        'kingpin.exe', 'ref_gl.dll', 'main/gamex86.dll', 'main/pak0.pak')]
    required.append(Path('track1.iso'))
    for path in required:
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f'Missing or empty required game file: {path}')
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=1) as z:
        for name in ('kingpin.exe', 'ref_gl.dll', 'kpintro.exe'):
            path = Path('game') / name
            if path.is_file():
                z.write(path, name)
        for path in sorted(Path('game/main').rglob('*')):
            if path.is_file():
                z.write(path, path.relative_to('game').as_posix())
        z.write('track1.iso', 'track1.iso')
        # install_game.sh creates symlinks; ZipFile.write follows them.
        # The data BIN is already represented by track1.iso; include only audio BINs.
        for path in sorted(Path('cd').glob('*.cue')):
            import re
            cue = path.read_text(encoding='latin-1')
            z.write(path, 'cd/' + path.name)
            for name, track in re.findall(r'FILE\s+"([^"]+)"\s+\w+(.*?)(?=\bFILE\s+"|\Z)', cue, re.S):
                if re.search(r'\bTRACK\s+\d+\s+AUDIO\b', track):
                    source = (path.parent / name).resolve()
                    if Path(name).name != name or not source.is_file():
                        raise ValueError('Invalid or missing audio BIN in CUE')
                    z.write(source, 'cd/' + name)
    print('Bundled game data:', output.stat().st_size, 'bytes')


if __name__ == '__main__':
    bundle(Path('build-android/assets/game-data.zip'))
