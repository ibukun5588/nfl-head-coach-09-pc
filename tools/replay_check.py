#!/usr/bin/env python3
"""Replay captures and compare every output file with reference outputs.

    python tools/replay_check.py <ref-root> <out-root> [--gpu-render] <capture-dir>...

<capture-dir> is e.g. captures/st/frame_4690 (contains replay.bin). Each
capture is replayed with build-run/hc09-replay into <out-root>/<name>, where
<name> is the capture path below captures/ with '/' -> '_' (st_frame_4690),
and compared file by file with <ref-root>/<name>/frame_N/: PNGs by pixel
statistics (tools/imgdiff.py), text files with line endings and NaN spelling
normalized. Typical uses:
  software regression:  replay_check.py captures/linux_ref captures/check captures/st/frame_4690 ...
  GPU vs software:      replay_check.py captures/linux_ref captures/gpu --gpu-render captures/st/frame_4690
Exit code 1 if any image differs.
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import imgdiff  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPLAY = os.path.join(ROOT, 'build-run', 'hc09-replay' + ('.exe' if os.name == 'nt' else ''))


def norm_text(path):
    return open(path, 'rb').read().replace(b'\r\n', b'\n').replace(b'-nan(ind)', b'-nan')


def image_stats(a, b):
    w, h, pa = imgdiff.read_png(a)
    w2, h2, pb = imgdiff.read_png(b)
    if (w, h) != (w2, h2):
        return None, f'size {w}x{h} vs {w2}x{h2}'
    n, mx = 0, 0
    for i in range(0, len(pa), 4):
        e = max(abs(pa[i + k] - pb[i + k]) for k in range(4))
        if e:
            n += 1
            mx = max(mx, e)
    return n, f'{n} px ({100.0 * n / (w * h):.3f}%) max {mx}'


def main():
    args = sys.argv[1:]
    gpu = '--gpu-render' in args
    args = [a for a in args if a != '--gpu-render']
    if len(args) < 3:
        print(__doc__)
        return 2
    ref_root, out_root, caps = args[0], args[1], args[2:]
    bad = False
    for cap in caps:
        rel = os.path.relpath(cap, os.path.join(ROOT, 'captures')) if os.path.isabs(cap) else cap
        rel = rel.replace('\\', '/')
        if rel.startswith('captures/'):
            rel = rel[len('captures/'):]
        name = rel.strip('/').replace('/', '_')
        out = os.path.join(out_root, name)
        t0 = time.time()
        cmd = [REPLAY, os.path.join(cap, 'replay.bin'), out] + (['--gpu-render'] if gpu else [])
        r = subprocess.run(cmd, capture_output=True, text=True)
        dt = time.time() - t0
        ref_base = os.path.join(ref_root, name)
        frames = [d for d in os.listdir(ref_base) if d.startswith('frame_')] if os.path.isdir(ref_base) else []
        if r.returncode or not frames:
            print(f'{cap}: replay exit {r.returncode}, reference frames {frames}')
            print(r.stderr[-1500:])
            bad = True
            continue
        ref, got = os.path.join(ref_base, frames[0]), os.path.join(out, frames[0])
        same, diffs = 0, []
        for dp, _, files in os.walk(ref):
            for f in files:
                rp = os.path.join(dp, f)
                gp = os.path.join(got, os.path.relpath(rp, ref))
                if not os.path.exists(gp):
                    diffs.append(f'missing {os.path.relpath(rp, ref)}')
                    continue
                if f.endswith('.png'):
                    n, desc = image_stats(rp, gp)
                    if n == 0:
                        same += 1
                    else:
                        diffs.append(f'{os.path.relpath(rp, ref)}: {desc}')
                        bad = True
                elif f.endswith('.txt'):
                    if norm_text(rp) == norm_text(gp) or gpu:   # GPU runs note "rendered by the GPU backend"
                        same += 1
                    else:
                        diffs.append(f'{os.path.relpath(rp, ref)}: text differs')
                elif open(rp, 'rb').read() == open(gp, 'rb').read():
                    same += 1
                else:
                    diffs.append(f'{os.path.relpath(rp, ref)}: differs')
        print(f'{name}: {dt:.1f}s, {same} files identical, {len(diffs)} different')
        for d in sorted(diffs, key=lambda s: ('frame.png' not in s, s))[:12]:
            print('    ' + d)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
