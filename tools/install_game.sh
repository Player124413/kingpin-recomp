#!/bin/bash
# Sets up the game files from your own Kingpin: Life of Crime CD (a BIN/CUE rip):
#   tools/install_game.sh "<path>/Kingpin - Life of Crime (USA).cue" [--low-violence]
#
# Creates (all git-ignored, never commit them):
#   track1.iso   the disc's data track (D:\ for the game)
#   cd/          links to the cue sheet and its .bin files (CD music; the game's CD check
#                also needs the disc's track lengths)
#   game/        the installed game: InstallShield groups from data1.cab, plus kpintro.exe
#                and this port's settings (extras/*.cfg)
# Needs: python3, unshield (brew install unshield / apt install unshield).
set -e
cd "$(dirname "$0")/.."
cue=$1
[ -f "$cue" ] || { echo "usage: $0 <disc.cue> [--low-violence]"; exit 1; }
violence=Mature_Files
[ "$2" = "--low-violence" ] && violence=Low_Violence_Files
command -v unshield >/dev/null || { echo "unshield is required (brew install unshield)"; exit 1; }

echo "== data track -> track1.iso"
[ -f track1.iso ] || python3 tools/disc.py iso "$cue" track1.iso

echo "== CD music -> cd/"
mkdir -p cd
cuedir=$(cd "$(dirname "$cue")" && pwd)
ln -sf "$cuedir/$(basename "$cue")" cd/
grep -o 'FILE "[^"]*"' "$cue" | sed 's/FILE "\(.*\)"/\1/' | while read -r f; do ln -sf "$cuedir/$f" cd/; done

echo "== installing from data1.cab -> game/"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for f in data1.hdr data1.cab; do python3 tools/disc.py get track1.iso "/kingpin/$f" "$tmp/$f"; done
unshield -d "$tmp/x" x "$tmp/data1.cab" >/dev/null
mkdir -p game
cp -R "$tmp/x/Common_Files/." "$tmp/x/$violence/." game/
cp "$tmp/x/Program_Executable_Files/kingpin.exe" "$tmp/x/Program_Executable_Files/ref_gl.dll" game/
cp -R "$tmp/x/Program_Executable_Files/main/." game/main/
cp "$tmp/x/Movies/kpintro.exe" game/

echo "== port settings"
cp extras/port.cfg extras/gamepad.cfg game/main/
grep -q "exec port.cfg" game/main/autoexec.cfg 2>/dev/null || printf '\nexec port.cfg\nexec gamepad.cfg\n' >> game/main/autoexec.cfg

echo "done. Next: generate the recompiled code (see README) and build."
