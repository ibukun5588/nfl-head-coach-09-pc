#!/usr/bin/env python3
"""Summarise tools/lowend_run.ps1 runs as Markdown table rows (docs/REPORT_LOWEND.md).

    py -3.11 tools/lowend_report.py <run dir> [<run dir> ...]

Per scene window (seconds after start; career: main menu 45-90, hub 400-455, Sim To 530-700; game: in play 540-700):
frames/s, the time between presented frames (median, 99th percentile, max) and the share of frames over 17.5 ms (a
missed 60 Hz frame), hc09.exe's CPU use in cores and its busiest thread, GPU 3D-engine use and NVIDIA board power.
The renderer's [native] lines come every 10 s of wall time from its start (taken as 2 s after the process start),
so a line's time is 2 + 10 k s; the samples (perf.csv) carry their own times.
"""
import csv
import os
import re
import statistics
import sys

WINDOWS = {
    'career': [('main menu', 45, 90), ('career hub', 400, 455), ('Sim To', 530, 700)],
    'game': [('in play', 540, 700)],
}
FPS = re.compile(r'^\[native\] frame \d+: ([\d.]+) frames/s')
FT = re.compile(r'^\[native\] frame times: median ([\d.]+) ms, 99% ([\d.]+) ms, max ([\d.]+) ms; (\d+) of (\d+) over 17.5 ms, (\d+) over 34 ms')


def main():
    rows = []
    for d in sys.argv[1:]:
        name = os.path.basename(os.path.normpath(d))
        scene = 'game' if name.endswith('-game') else 'career'
        fps, ft = [], []
        with open(os.path.join(d, 'run.log'), encoding='utf-8', errors='replace') as f:
            for line in f:
                m = FPS.match(line)
                if m:
                    fps.append((2 + 10 * (len(fps) + 1), float(m.group(1))))
                    continue
                m = FT.match(line)
                if m:
                    ft.append((2 + 10 * len(fps), [float(x) for x in m.groups()]))
        samples = []
        p = os.path.join(d, 'perf.csv')
        if os.path.exists(p):
            with open(p, encoding='utf-8') as f:
                for r in csv.DictReader(f):
                    try:
                        t = float(r['time'])
                    except ValueError:
                        continue
                    gpu = sum(float(x.split('=')[1]) for x in r['gpu3d_by_adapter'].split()) if r['gpu3d_by_adapter'] else 0.0
                    samples.append((t, float(r['cpu_cores']), float(r['thread1'] or 0), gpu,
                                    float(r['nv_watts']) if r['nv_watts'] else float('nan')))
        for label, a, b in WINDOWS[scene]:
            fw = [v for t, v in fps if a <= t <= b]
            tw = [v for t, v in ft if a <= t <= b]
            sw = [s for s in samples if a <= s[0] <= b]
            if not fw:
                rows.append(f'| {name} | {label} | (no data) |')
                continue
            n = sum(v[4] for v in tw) or 1
            over = sum(v[3] for v in tw)
            mean = lambda xs: statistics.fmean(xs) if xs else float('nan')
            rows.append('| {} | {} | {:.1f} | {:.1f} / {:.1f} / {:.1f} | {:.1f}% | {:.2f} | {:.2f} | {:.0f}% | {:.0f} |'.format(
                name, label, mean(fw), mean([v[0] for v in tw]), max([v[1] for v in tw] or [0]), max([v[2] for v in tw] or [0]),
                100.0 * over / n, mean([s[1] for s in sw]), mean([s[2] for s in sw]), mean([s[3] for s in sw]),
                mean([s[4] for s in sw])))
    print('| run | scene | frames/s | frame time median / 99% / max (ms) | frames > 17.5 ms | CPU (cores) | busiest thread | GPU 3D | NVIDIA W |')
    print('|---|---|---|---|---|---|---|---|---|')
    for r in rows:
        print(r)


if __name__ == '__main__':
    main()
