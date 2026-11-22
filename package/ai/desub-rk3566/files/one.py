#!/usr/bin/env python3
"""Process ONE frame and save before/after PNGs, for fast quality iteration.

  python3 one.py <video> <seconds> <out.png> [--white W] [--band B] [--dilate D] [--ring R]
"""
import argparse
import os
import subprocess
import sys

import importlib.util
import numpy as np

DESUB_DIR = os.environ.get('DESUB_DIR', os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, DESUB_DIR)
# 主脚本名带横线（desub-rk3566.py），不能直接 import，按路径加载
_spec = importlib.util.spec_from_file_location(
    'desub_rk3566', os.path.join(DESUB_DIR, 'desub-rk3566.py'))
D = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(D)

SZ = 512
FF = '/usr/bin/ffmpeg-rkrga'
MODEL = os.path.join(DESUB_DIR, 'migan_noclip.rknn')
BIN = os.path.join(DESUB_DIR, 'migan_rknn')
LOADER = '/opt/glibc/ld-linux-aarch64.so.1'
LIBPATH = '/opt/glibc:/opt/rknpu'

ap = argparse.ArgumentParser()
ap.add_argument('video')
ap.add_argument('secs', type=float)
ap.add_argument('out')
ap.add_argument('--white', type=int, default=45, help='局部对比度阈值')
ap.add_argument('--lc-radius', type=int, default=12)
ap.add_argument('--band', type=float, default=0.78)
ap.add_argument('--dilate', type=int, default=10)
ap.add_argument('--ring', type=int, default=28)
ap.add_argument('--flat', type=float, default=13.0)
ap.add_argument('--noise', action='store_true')
ap.add_argument('--feather', type=int, default=6)
ap.add_argument('--margin', type=int, default=8)
ap.add_argument('--ring-pad', type=int, default=28)
ap.add_argument('--normal', action='store_true', help='ch0 用 0.5-mask（默认用反极性）')
a = ap.parse_args()

meta = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                       'stream=width,height', '-of', 'csv=p=0', a.video],
                      capture_output=True, text=True).stdout.strip().replace('\n', ',')
if not meta:
    meta = subprocess.run([FF, '-i', a.video], capture_output=True, text=True).stderr
    import re
    m = re.search(r'(\d{2,5})x(\d{2,5})', meta)
    W, H = int(m.group(1)), int(m.group(2))
else:
    W, H = int(meta.split(',')[0]), int(meta.split(',')[1])
raw = subprocess.run([FF, '-v', 'error', '-ss', str(a.secs), '-i', a.video, '-frames:v', '1',
                      '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True).stdout
frame = np.frombuffer(raw[:W * H * 3], np.uint8).reshape(H, W, 3)

big = D.resize(frame, SZ, SZ)
g = D.gray_of(frame)
gs = D.resize(g[..., None], SZ, SZ)[..., 0]
hole = D.refine_rows(D.text_mask_lc(gs, a.band, a.white, a.lc_radius))
print('mask px(raw)=%d' % hole.sum())
hole = D.dilate(D.dilate(hole, a.dilate), a.margin)
print('mask px(dilated)=%d' % hole.sum())

keep = np.where(hole, 0, 255).astype(np.uint8)
erased = big.copy()
erased[hole] = 127
open('/tmp/one_i.rgb', 'wb').write(erased.tobytes())
open('/tmp/one_m.gray', 'wb').write(keep.tobytes())

cmd = [LOADER, BIN, MODEL, '/tmp/one_i.rgb', '/tmp/one_m.gray', '/tmp/one_o.rgb']
if a.normal:
    cmd.append('normal')
env = dict(os.environ, LD_LIBRARY_PATH=LIBPATH)
r = subprocess.run(cmd, capture_output=True, text=True, env=env)
print('harness rc=%d %s' % (r.returncode, r.stderr.strip().splitlines()[:2]))

gen = np.fromfile('/tmp/one_o.rgb', dtype=np.uint8).reshape(SZ, SZ, 3)
ring = D.dilate(hole, a.dilate + a.ring) & ~hole
ys, xs = np.where(hole)
pad = a.dilate + a.ring + a.ring_pad
kb = np.zeros_like(ring)
kb[max(0, ys.min() - pad):min(SZ, ys.max() + pad),
   max(0, xs.min() - pad):min(SZ, xs.max() + pad)] = True
ring &= kb
print('ring px=%d  环带std=%s' % (ring.sum(), big[ring].astype(np.float32).std(0).round(1)))
_lum = big[ring].astype(np.float32).mean(1)
_med = float(np.median(_lum)); _mad = float(np.median(np.abs(_lum - _med)))
_keep = np.abs(_lum - _med) < 3.0 * max(_mad, 3.0)
_ry, _rx = np.nonzero(ring)
if _keep.sum() > 100:
    print('过滤后 std=%s  (保留 %d/%d)' % (
        big[_ry[_keep], _rx[_keep]].astype(np.float32).std(0).round(1), _keep.sum(), len(_keep)))
gen, hole = D.match_background(gen, big, hole, ring, a.flat, a.noise)

small = D.resize(gen, W, H)
hw = D.resize(hole.astype(np.uint8)[..., None] * 255, W, H)[..., 0] > 127
out = D.feather_blend(frame, small, hw, a.feather) if a.feather > 0 else (lambda b,f,h: np.where(h[...,None], f, b))(frame, small, hw)

# 存 PNG（ffmpeg 收 rgb24 再编码，避免依赖 PIL）
open('/tmp/one_in.rgb', 'wb').write(frame.tobytes())
open('/tmp/one_out.rgb', 'wb').write(out.tobytes())
for src, dst in (('/tmp/one_in.rgb', a.out.replace('.png', '_before.png')),
                 ('/tmp/one_out.rgb', a.out)):
    subprocess.run([FF, '-y', '-v', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                    '-s', '%dx%d' % (W, H), '-i', src, '-frames:v', '1', dst])
print('saved', a.out)
