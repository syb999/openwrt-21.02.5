#!/usr/bin/env python3
"""Micro-benchmark of the per-frame numpy steps."""
import importlib.util
import os
import sys
import time

import numpy as np

DESUB_DIR = os.environ.get('DESUB_DIR', os.path.dirname(os.path.abspath(__file__)))
_spec = importlib.util.spec_from_file_location(
    'desub_rk3566', os.path.join(DESUB_DIR, 'desub-rk3566.py'))
D = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(D)

SZ = 512
frame = np.random.randint(0, 255, (432, 576, 3), np.uint8)
big = D.resize(frame, SZ, SZ)


def t(name, fn, n=5):
    fn()
    t0 = time.time()
    for _ in range(n):
        r = fn()
    dt = (time.time() - t0) / n
    print('%-28s %6.1f ms' % (name, dt * 1000))
    return r


t('resize 432x576 -> 512', lambda: D.resize(frame, SZ, SZ))
t('resize 512 -> 432x576', lambda: D.resize(big, 576, 432))
t('gray_of', lambda: D.gray_of(frame))
gs = D.resize(D.gray_of(frame)[..., None], SZ, SZ)[..., 0]
t('resize gray -> 512', lambda: D.resize(D.gray_of(frame)[..., None], SZ, SZ))
t('text_mask_lc', lambda: D.text_mask_lc(gs, 0.78, 45, 12))
m = D.text_mask_lc(gs, 0.78, 45, 12)
t('refine_rows', lambda: D.refine_rows(m))
t('dilate r=10', lambda: D.dilate(m, 10))
t('dilate r=34', lambda: D.dilate(m, 34))
h = D.dilate(D.dilate(m, 10), 8)
t('box_blur 512 r=30', lambda: D.box_blur(gs.astype(np.float32), 30))

ring = D.dilate(h, 24) & ~h
t('ring_std', lambda: D.ring_std(big, ring))
t('match_background(flat)', lambda: D.match_background(big.copy(), big, h, ring, 13.0, False))
gen = big.copy()
t('feather_blend', lambda: D.feather_blend(frame, D.resize(gen, 576, 432), np.ones((432, 576), bool), 6))
