#!/bin/bash
# Generates the optional HD textures from your own installed game (game/main) into game/main/hd/:
#   tools/make_hd_textures.sh [--only <path substring>] [--force]
# Downloads the Real-ESRGAN x4plus weights (BSD-3-Clause, github.com/xinntao/Real-ESRGAN) on first
# use. Takes a while (all 3000+ textures: under an hour on an Apple M-series GPU). The results are
# derived from the game's art: keep them local, do not share or commit them.
set -e
cd "$(dirname "$0")/.."
[ -d game/main ] || { echo "install the game first (tools/install_game.sh)"; exit 1; }
[ -x .venv/bin/python ] || python3 -m venv .venv
.venv/bin/pip install -q -r requirements-hd.txt
model=tools/models/RealESRGAN_x4plus.pth
sum=4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1
if [ ! -f "$model" ]; then
  mkdir -p tools/models
  echo "downloading the Real-ESRGAN x4plus weights"
  curl -L --fail --progress-bar -o "$model.part" https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth
  mv "$model.part" "$model"
fi
echo "$sum  $model" | shasum -a 256 -c --quiet || { echo "unexpected model file: $model"; exit 1; }
exec .venv/bin/python tools/upscale_textures.py "$@"
