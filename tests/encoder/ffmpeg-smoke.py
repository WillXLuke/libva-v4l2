#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Hardware H.264 encode smoke test; uses the selected libva driver.

Run on the Iris board: python3 tests/encoder/ffmpeg-smoke.py /path/to/results
Set LIBVA_DRIVER_NAME and LIBVA_DRIVERS_PATH to test an uninstalled build.
Requires FFmpeg with h264_vaapi and software H.264 decoding, plus ffprobe.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

root = Path(sys.argv[1]).resolve()
root.mkdir(parents=True, exist_ok=True)
summary = []

def run(args, name, timeout=90, env=None):
    start = time.monotonic()
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True, timeout=timeout, env=env)
    (root / (name + '.log')).write_text(p.stderr)
    if p.returncode:
        raise RuntimeError(f'{name} failed ({p.returncode}); see {root / (name + ".log")}')
    return p.stdout, time.monotonic() - start

cases = [
    ('baseline-cqp', 640, 480, 'constrained_baseline', ['-qp', '24'], 90, 'mp4'),
    ('main-cbr', 640, 480, 'main', ['-rc_mode', 'CBR', '-b:v', '2M'], 90, 'mp4'),
    ('high-vbr', 640, 480, 'high', ['-rc_mode', 'VBR', '-b:v', '2M', '-maxrate', '3M'], 90, 'mp4'),
    ('high-cqp', 640, 480, 'high', ['-qp', '24'], 90, 'mp4'),
    ('crop', 642, 482, 'high', ['-qp', '24'], 60, 'mp4'),
    ('1080p', 1920, 1080, 'high', ['-qp', '24'], 120, 'mp4'),
    ('4k', 3840, 2160, 'high', ['-qp', '24'], 60, 'mp4'),
    ('one-frame', 128, 128, 'high', ['-qp', '24'], 1, 'mp4'),
]
for name, w, h, profile, rc, frames, container in cases:
    raw = root / f'{w}x{h}-{frames}.nv12'
    if not raw.exists():
        run(['ffmpeg', '-nostdin', '-v', 'warning', '-f', 'lavfi', '-i',
             f'testsrc2=size={w}x{h}:rate=30,format=nv12', '-frames:v', str(frames),
             '-f', 'rawvideo', '-y', str(raw)], name + '-source')
    env = dict(os.environ, IRIS_VAAPI_DEBUG='1')
    output = root / (name + '.' + container)
    _, elapsed = run(['ffmpeg', '-nostdin', '-v', 'verbose', '-vaapi_device', '/dev/dri/renderD128',
        '-f', 'rawvideo', '-pixel_format', 'nv12', '-video_size', f'{w}x{h}', '-framerate', '30',
        '-i', str(raw), '-vf', 'hwupload', '-c:v', 'h264_vaapi', '-profile:v', profile,
        '-bf', '0', '-g', '30', *rc, '-frames:v', str(frames), '-y', str(output)], name, env=env)
    info, _ = run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
        '-show_entries', 'stream=codec_name,profile,width,height,nb_read_frames,extradata_size:frame=key_frame,pict_type',
        '-of', 'json', str(output)], name + '-probe')
    info = json.loads(info)
    stream = info['streams'][0]
    assert (stream['width'], stream['height'], int(stream['nb_read_frames'])) == (w, h, frames), info
    assert stream['codec_name'] == 'h264', info
    assert stream['profile'] == {'constrained_baseline': 'Constrained Baseline', 'main': 'Main', 'high': 'High'}[profile], info
    assert sum(f['key_frame'] for f in info['frames']) == (frames + 29) // 30, info
    assert all(f['pict_type'] in ('I', 'P') for f in info['frames']), info
    run(['ffmpeg', '-nostdin', '-hide_banner', '-xerror', '-i', str(output),
         '-f', 'rawvideo', '-pixel_format', 'nv12', '-video_size', f'{w}x{h}', '-framerate', '30',
         '-i', str(raw), '-lavfi', 'ssim', '-frames:v', str(frames), '-f', 'null', '-'], name + '-quality')
    log = (root / (name + '-quality.log')).read_text()
    quality = float(re.search(r'All:([0-9.]+)', log)[1])
    assert quality > 0.97, (name, quality)
    paths = re.findall(r'encoder input path=(direct|staging|cpu-mmap)',
                       (root / (name + '.log')).read_text())
    assert len(paths) == frames, (name, len(paths), frames)
    row = dict(name=name, frames=frames, seconds=round(elapsed, 3),
               fps=round(frames / elapsed, 1), ssim=quality,
               input_paths={path: paths.count(path) for path in sorted(set(paths))})
    summary.append(row)
    print(json.dumps(row), flush=True)
    (root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print('PASS: all encoded streams decoded, frame counts/keyframes/profiles/dimensions/quality verified.')
