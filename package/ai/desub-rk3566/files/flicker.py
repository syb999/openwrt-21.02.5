#!/usr/bin/env python3
"""Measure frame-to-frame flicker of the subtitle band.

  python3 flicker.py <video> <start_sec> <nframes>
"""
import subprocess
import sys

import numpy as np

FF = '/usr/bin/ffmpeg-rkrga'

video, start, n = sys.argv[1], float(sys.argv[2]), int(sys.argv[3])
probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0',
                        '-show_entries', 'stream=width,height', '-of', 'csv=p=0', video],
                       capture_output=True, text=True).stdout.strip().split(',')
W, H = int(probe[0]), int(probe[1])

raw = subprocess.run([FF, '-v', 'error', '-ss', str(start), '-i', video, '-frames:v', str(n),
                      '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True).stdout
fsz = W * H * 3
frames = [np.frombuffer(raw[i * fsz:(i + 1) * fsz], np.uint8).reshape(H, W, 3)
          for i in range(min(n, len(raw) // fsz))]

y0, y1 = int(H * 0.86), int(H * 0.97)      # 字幕带内（避开边缘）
means = [f[y0:y1, 40:W - 40].reshape(-1, 3).mean(0) for f in frames]

print('frame        mean RGB              delta vs prev')
prev = None
for i, m in enumerate(means):
    d = '-' if prev is None else '%.2f' % np.abs(m - prev).max()
    print('%3d   %s   %s' % (i, m.round(1), d))
    prev = m

if len(means) > 1:
    arr = np.array(means)
    print('\n帧间最大跳变 %.2f   总标准差 %s' %
          (np.abs(np.diff(arr, axis=0)).max(), arr.std(0).round(2)))
