#!/usr/bin/env python3
"""Compare two RKNN models on identical real inputs.

  python3 cmp_models.py <modelA.rknn> <modelB.rknn> [t1 t2 ...]
"""
import os
import subprocess
import sys

import importlib.util
import numpy as np

DESUB_DIR = os.environ.get('DESUB_DIR', os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, DESUB_DIR)
_spec = importlib.util.spec_from_file_location(
    'desub_rk3566', os.path.join(DESUB_DIR, 'desub-rk3566.py'))
D = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(D)

SZ = 512
FF = '/usr/bin/ffmpeg-rkrga'
LOADER = '/opt/glibc/ld-linux-aarch64.so.1'
BIN = os.path.join(DESUB_DIR, 'migan_rknn')
ENV = dict(os.environ, LD_LIBRARY_PATH='/opt/glibc:/opt/rknpu')

vid = '/opt/mp4/03.mp4'
probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                        '-show_entries', 'stream=width,height', '-of', 'csv=p=0', vid],
                       capture_output=True, text=True).stdout.strip().split(',')
W, H = int(probe[0]), int(probe[1])

models = [sys.argv[1], sys.argv[2]]
times = [float(x) for x in sys.argv[3:]] or [2.2, 4.0, 5.4, 7.0]


def run(model, p_img, p_msk, p_out):
    subprocess.run([LOADER, BIN, model, p_img, p_msk, p_out, 'inv'],
                   env=ENV, capture_output=True)


for t in times:
    raw = subprocess.run([FF, '-v', 'error', '-ss', str(t), '-i', vid, '-frames:v', '1',
                          '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         capture_output=True).stdout
    frame = np.frombuffer(raw, np.uint8).reshape(H, W, 3)
    g = D.gray_of(frame)
    gs = D.resize(g[..., None], SZ // 2, SZ // 2)[..., 0]
    hole = D.text_mask_lc(gs, 0.78, 45, 6)
    hole = np.repeat(np.repeat(D.refine_rows(hole), 2, 0), 2, 1)[:SZ, :SZ]
    if hole.sum() < 50:
        print('%.1fs: 无字幕，跳过' % t)
        continue
    hole = D.dilate(hole, 10)
    big = D.resize(frame, SZ, SZ)
    erased = big.copy()
    erased[hole] = 127
    keep = np.where(hole, 0, 255).astype(np.uint8)
    open('/tmp/cmp_i.rgb', 'wb').write(erased.tobytes())
    open('/tmp/cmp_m.gray', 'wb').write(keep.tobytes())

    outs = []
    for k, m in enumerate(models):
        p = '/tmp/cmp_o%d.rgb' % k
        run(m, '/tmp/cmp_i.rgb', '/tmp/cmp_m.gray', p)
        outs.append(np.frombuffer(open(p, 'rb').read(), np.uint8)
                    .reshape(SZ, SZ, 3).astype(np.int16))
    d = np.abs(outs[0] - outs[1])
    print('%.1fs  最大差=%3d  平均差=%.3f  洞内平均差=%.3f'
          % (t, d.max(), d.mean(), d[hole].mean()))
