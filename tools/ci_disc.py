#!/usr/bin/env python3
"""Download a user-owned BIN/CUE ZIP for the manual Android CI build."""
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import tempfile
from urllib.parse import urlparse
from urllib.request import Request, urlopen
import zipfile


def main():
    url = os.environ.get('GAME_ARCHIVE_URL', '').strip()
    parsed = urlparse(url)
    host = parsed.hostname
    if parsed.scheme != 'https' or host not in {'drive.google.com', 'huggingface.co'}:
        raise ValueError('Set GAME_ARCHIVE_URL to an HTTPS Google Drive or Hugging Face ZIP URL')
    destination = Path('disc')
    destination.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        archive = Path(tmp) / 'disc.zip'
        if host == 'drive.google.com':
            import gdown
            if not gdown.download(url, str(archive), fuzzy=True, quiet=True):
                raise ValueError('Google Drive download failed; enable access by link')
        else:
            headers = {}
            token = os.environ.get('HF_TOKEN', '').strip()
            if token:
                headers['Authorization'] = f'Bearer {token}'
            with urlopen(Request(url, headers=headers), timeout=120) as response:
                with archive.open('wb') as output:
                    shutil.copyfileobj(response, output)
        with zipfile.ZipFile(archive) as z:
            # Validate every member before extraction (including non-game files).
            for entry in z.infolist():
                path = PurePosixPath(entry.filename)
                if (path.is_absolute() or '..' in path.parts or '\\' in entry.filename
                        or stat.S_ISLNK(entry.external_attr >> 16)):
                    raise ValueError('Unsafe path in disc ZIP')
            if sum(e.file_size for e in z.infolist()) > 4 * 1024**3:
                raise ValueError('Disc ZIP exceeds the 4 GiB uncompressed limit')
            z.extractall(destination)
    cues = list(destination.rglob('*'))
    cues = [p for p in cues if p.is_file() and p.suffix.lower() == '.cue']
    if len(cues) != 1:
        raise ValueError('ZIP must contain exactly one CUE sheet and its BIN files')
    for name in re.findall(r'FILE\s+"([^"]+)"', cues[0].read_text(encoding='latin-1')):
        path = (cues[0].parent / name).resolve()
        if not path.is_relative_to(destination.resolve()) or not path.is_file():
            raise ValueError('CUE references a missing BIN or a path outside the disc folder')
    print('Disc downloaded and validated.')


if __name__ == '__main__':
    # Download errors may contain signed URLs or tokens: do not put them in CI logs.
    try:
        main()
    except Exception:
        raise SystemExit('Disc setup failed. Check the secret URL, access permissions and ZIP contents.')
