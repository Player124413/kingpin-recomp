#!/bin/bash
# Copies your game files to the connected Android device/emulator (adb):
#   android/push_data.sh <installed game dir> <track1.iso> [<cue sheet>]
# The game dir holds kingpin.exe, ref_gl.dll, main/ and (for the intro movie) kpintro.exe. With a cue sheet, its audio track .bin
# files are copied too (CD music). The app must have been installed first.
set -e
ADB=${ADB:-$HOME/Library/Android/sdk/platform-tools/adb}
D=/sdcard/Android/data/com.blounty.kingpin/files
game=$1 iso=$2 cue=$3
[ -f "$game/kingpin.exe" ] && [ -f "$iso" ] || { echo "usage: $0 <game dir> <track1.iso> [cue]"; exit 1; }
$ADB shell mkdir -p $D/cd
$ADB push "$game/kingpin.exe" "$game/ref_gl.dll" $D/
[ -f "$game/kpintro.exe" ] && $ADB push "$game/kpintro.exe" $D/   # the intro movie
$ADB push "$game/main" $D/
$ADB push "$iso" $D/track1.iso
if [ -n "$cue" ]; then
  $ADB push "$cue" $D/cd/
  dir=$(dirname "$cue")
  grep -o 'FILE "[^"]*"' "$cue" | sed 's/FILE "\(.*\)"/\1/' | while read -r f; do
    if grep -A1 "FILE \"$f\"" "$cue" | grep -q AUDIO; then $ADB push "$dir/$f" $D/cd/; fi
  done
fi
# Directories adb creates belong to the shell user: let the app into them.
$ADB shell "find $D -mindepth 1 -type d -exec chmod 777 {} +"
echo "done"
