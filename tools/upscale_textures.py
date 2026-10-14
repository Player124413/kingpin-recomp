#!/usr/bin/env python3
"""Upscales the game's textures 4x with Real-ESRGAN (GPU via PyTorch: Apple MPS or CUDA).

usage: upscale_textures.py [--only substring] [--force]   (or tools/make_hd_textures.sh, which sets it up)

Reads game/main/{textures,models,sprites}/**/*.tga and writes game/main/hd/<same path>.png,
which the runtime substitutes at upload time (see runtime/win32/gl_textures.cpp). The generated
images are derived from the user's own game data and stay local.

World textures tile, so they are padded with wrapped-around pixels before upscaling (keeps the
edges seamless); model skins are padded with their edge pixels. Alpha is upscaled separately:
smoothly, and re-thresholded for cut-out textures (fences, grates) whose alpha is only 0/255.
"""
import os
import sys

import numpy as np
import torch
from PIL import Image
from spandrel import ModelLoader

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
MAIN = os.path.join(ROOT, 'game', 'main')
OUT = os.path.join(MAIN, 'hd')
MODEL = os.path.join(ROOT, 'tools', 'models', 'RealESRGAN_x4plus.pth')
PAD = 12


def upscale_rgb(model, dev, rgb):
    t = torch.from_numpy(rgb.astype(np.float32) / 255.0).permute(2, 0, 1)[None].to(dev)
    with torch.no_grad():
        o = model(t).clamp(0, 1)
    return (o[0].permute(1, 2, 0).cpu().numpy() * 255.0 + 0.5).astype(np.uint8)


def color_correct(orig_rgb, up_rgb, tiling):
    """Keeps the original's colours: removes the low-frequency difference between the original and
    the upscaled image (measured at the original resolution) so the AI only adds detail."""
    h, w = orig_rgb.shape[:2]
    up = up_rgb.astype(np.float32)
    down = up.reshape(h, 4, w, 4, 3).mean(axis=(1, 3))
    diff = orig_rgb.astype(np.float32) - down
    # Upsample the correction smoothly (with wrap-around padding for tiling textures).
    pad = 2
    dp = np.pad(diff, ((pad, pad), (pad, pad), (0, 0)), mode='wrap' if tiling else 'edge')
    chans = []
    for ch in range(3):
        img = Image.fromarray(dp[:, :, ch], 'F').resize(((w + 2 * pad) * 4, (h + 2 * pad) * 4), Image.BICUBIC)
        chans.append(np.array(img)[4 * pad:4 * (pad + h), 4 * pad:4 * (pad + w)])
    return np.clip(up + np.dstack(chans) + 0.5, 0, 255).astype(np.uint8)


def process(model, dev, src, dst):
    im = Image.open(src).convert('RGBA')
    a = np.array(im)
    h, w = a.shape[:2]
    tiling = os.sep + 'textures' + os.sep in src
    mode = 'wrap' if tiling else 'edge'
    pad = min(PAD, h, w)
    rgb = np.pad(a[:, :, :3], ((pad, pad), (pad, pad), (0, 0)), mode=mode)
    up = upscale_rgb(model, dev, rgb)[4 * pad:4 * (pad + h), 4 * pad:4 * (pad + w)]
    up = color_correct(a[:, :, :3], up, tiling)
    alpha = a[:, :, 3]
    if alpha.min() < 255:
        al = Image.fromarray(alpha).resize((4 * w, 4 * h), Image.BICUBIC)
        al = np.array(al)
        if set(np.unique(alpha)) <= {0, 255}:
            al = np.where(al >= 128, 255, 0).astype(np.uint8)
        out = np.dstack([up, al])
    else:
        out = np.dstack([up, np.full(up.shape[:2], 255, np.uint8)])
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    Image.fromarray(out, 'RGBA').save(dst, optimize=False)


def fix_existing():
    """Applies colour correction to images upscaled before it existed."""
    n = 0
    for d, _, fs in os.walk(OUT):
        for f in fs:
            dst = os.path.join(d, f)
            rel = os.path.splitext(os.path.relpath(dst, OUT))[0]
            src = next((os.path.join(MAIN, rel + e) for e in ('.tga', '.TGA') if os.path.exists(os.path.join(MAIN, rel + e))), None)
            if not src:
                continue
            a = np.array(Image.open(src).convert('RGBA'))
            hd = np.array(Image.open(dst).convert('RGBA'))
            if hd.shape[0] != 4 * a.shape[0] or hd.shape[1] != 4 * a.shape[1]:
                continue
            rgb = color_correct(a[:, :, :3], hd[:, :, :3], os.sep + 'textures' + os.sep in src)
            Image.fromarray(np.dstack([rgb, hd[:, :, 3]]), 'RGBA').save(dst)
            n += 1
    print('colour-corrected %d images' % n)


def main():
    if '--fix-color' in sys.argv:
        fix_existing()
        return
    only = None
    force = '--force' in sys.argv
    if '--only' in sys.argv:
        only = sys.argv[sys.argv.index('--only') + 1]
    dev = 'mps' if torch.backends.mps.is_available() else 'cuda' if torch.cuda.is_available() else 'cpu'
    model = ModelLoader().load_from_file(MODEL).model.eval().to(dev)
    files = []
    for sub in ('textures', 'models', 'sprites'):
        for d, _, fs in os.walk(os.path.join(MAIN, sub)):
            for f in fs:
                if f.lower().endswith('.tga'):
                    files.append(os.path.join(d, f))
    files.sort()
    if only:
        files = [f for f in files if only in f]
    done = 0
    for k, src in enumerate(files):
        rel = os.path.relpath(src, MAIN)
        dst = os.path.join(OUT, os.path.splitext(rel)[0] + '.png')
        if os.path.exists(dst) and not force:
            continue
        try:
            process(model, dev, src, dst)
            done += 1
        except Exception as e:  # odd files: keep the original
            print('skip %s: %s' % (rel, e))
        if done % 100 == 0 and done:
            print('%d/%d' % (k + 1, len(files)), flush=True)
    print('upscaled %d textures (%d candidates) on %s' % (done, len(files), dev))


main()
