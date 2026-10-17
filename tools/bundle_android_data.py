#!/usr/bin/env python3
"""Package locally installed, user-owned game data for a personal Android APK."""
from pathlib import Path
import zipfile


def resolve_game_file(path):
    """Windows disc filenames are case-insensitive; Linux CI filenames are not."""
    path = Path(path)
    current = Path('.')
    for part in path.parts:
        if not current.is_dir():
            return path
        matches = [p for p in current.iterdir() if p.name.casefold() == part.casefold()]
        if len(matches) > 1:
            raise ValueError(f'Ambiguous case-insensitive game path: {path}')
        if not matches:
            return path
        current = matches[0]
    return current


def bundle(output, compression=zipfile.ZIP_DEFLATED):
    required = [Path('game') / p for p in (
        'kingpin.exe', 'ref_gl.dll', 'main/gamex86.dll', 'main/pak0.pak')]
    required.append(Path('track1.iso'))
    for path in required:
        path = resolve_game_file(path)
        if not path.is_file() or not path.stat().st_size:
            raise ValueError(f'Missing or empty required game file: {path}')
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, 'w', compression=compression) as z:
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


def export_directory(output):
    """Stage files directly so Actions creates one ZIP, not a ZIP inside a ZIP."""
    import tempfile
    if output.exists():
        raise ValueError(f'Export destination already exists: {output}')
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / 'game-data.zip'
        bundle(archive, compression=zipfile.ZIP_STORED)
        # This archive is generated above from our own validated paths.
        with zipfile.ZipFile(archive) as z:
            z.extractall(output)


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export-directory', type=Path)
    args = parser.parse_args()
    if args.export_directory:
        export_directory(args.export_directory)
    else:
        bundle(Path('build-android/assets/game-data.zip'))
