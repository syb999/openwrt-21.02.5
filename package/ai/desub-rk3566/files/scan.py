#!/usr/bin/env python3
"""Scan a whole video for temporal jumps inside the subtitle band.

  python3 scan.py <video> [band_top_frac]
"""
import subprocess
import sys

import numpy as np

FF = '/usr/bin/ffmpeg-rkrga'

video = sys.argv[1]
btop = float(sys.argv[2]) if len(sys.argv) > 2 else 0.86

probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                        '-show_entries', 'stream=width,height,r_frame_rate',
                        '-of', 'csv=p=0', video], capture_output=True, text=True).stdout.strip().split(',')
W, H = int(probe[0]), int(probe[1])
num, den = probe[2].split('/')
fps = float(num) / float(den)

dec = subprocess.Popen([FF, '-v', 'error', '-i', video, '-f', 'rawvideo',
                        '-pix_fmt', 'rgb24', '-'], stdout=subprocess.PIPE)
fsz = W * H * 3
y0, y1 = int(H * btop), int(H * 0.99)

means, i = [], 0
while True:
    buf = dec.stdout.read(fsz)
    if len(buf) < fsz:
        break
    f = np.frombuffer(buf, np.uint8).reshape(H, W, 3)
    means.append(f[y0:y1, 30:W - 30].reshape(-1, 3).mean(0))
    i += 1
dec.stdout.close()
dec.wait()

arr = np.array(means)
d = np.abs(np.diff(arr, axis=0)).max(1)
order = np.argsort(d)[::-1][:12]
print('帧数=%d  fps=%.2f' % (len(arr), fps))
print('跳变最大的 12 个时刻（帧号 / 秒 / 跳变值 / 该帧均值）:')
for k in sorted(order):
    print('  #%3d  %.2fs  jump=%.2f  mean=%s' % (k + 1, (k + 1) / fps, d[k], arr[k + 1].round(1)))
print('全局: 中位跳变 %.2f  99%%分位 %.2f  最大 %.2f' %
      (np.median(d), np.percentile(d, 99), d.max()))
