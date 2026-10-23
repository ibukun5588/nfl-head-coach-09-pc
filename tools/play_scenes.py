#!/usr/bin/env python3
"""Play frame rate of a scripted game, per scene weight (docs/REPORT_GAME_CPU.md 7.1).

The scripted game (docs/HANDOFF_144.md, test bench) does not show the same scenes every run, so a
plain mean compares different things. This buckets the play part by draws per frame (crowd close-ups
are the heaviest) from the log's lines:
  [native] frame N: F frames/s; renderer thread R ms/frame ...; totals: D draws ...   (every 10 s)
  [pacing] swN 5.0 s: F flips/s ...                                                   (every 5 s)
Play = from the swap of the press at --from-t (default 398 s: the load into the game) + --skip swaps.

    py -3.11 tools/play_scenes.py game-a.log game-b.log [--from-t 398] [--skip 900] [--windows]
"""
import re
import statistics
import sys

BUCKETS = [(0, 1000, '< 1000'), (1000, 1500, '1000-1500'), (1500, 2000, '1500-2000'),
           (2000, 3000, '2000-3000'), (3000, 1 << 30, '>= 3000')]


def parse(log, from_t, skip):
    start = None
    native, pacing = [], []
    prev = None
    for line in open(log, errors='replace'):
        m = re.match(r'\[input\] t=([\d.]+)s swap (\d+) press', line)
        if m and start is None and abs(float(m.group(1)) - from_t) < 0.5:
            start = int(m.group(2)) + skip
        m = re.match(r'\[native\] frame (\d+): ([\d.]+) frames/s; renderer thread ([\d.]+) ms.*?totals: (\d+) draws', line)
        if m:
            fr, fps, rt, dr = int(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4))
            if prev and fr > prev[0]:
                native.append((fr, fps, (dr - prev[1]) / (fr - prev[0]), rt))
            prev = (fr, dr)
        m = re.match(r'\[pacing\] sw(\d+) [\d.]+ s: ([\d.]+) flips/s', line)
        if m:
            pacing.append((int(m.group(1)), float(m.group(2))))
    if start is None:
        return None, [], []
    return start, [w for w in native if w[0] >= start], [w for w in pacing if w[0] >= start]


def main():
    args = sys.argv[1:]
    def opt(name, default):
        return type(default)(args[args.index(name) + 1]) if name in args else default
    from_t, skip, show = opt('--from-t', 398.0), opt('--skip', 900), '--windows' in args
    logs = [a for i, a in enumerate(args) if not a.startswith('--') and (i == 0 or args[i - 1] not in ('--from-t', '--skip'))]
    for log in logs:
        start, native, pacing = parse(log, from_t, skip)
        if start is None or not native:
            print(f'{log}: no play part found (press at t={from_t:g}?)')
            continue
        fps = [f for _, f in pacing]
        print(f'{log}: play from swap {start}')
        if fps:
            n = len(fps)
            below = lambda x: 100.0 * sum(1 for f in fps if f < x) / n
            print(f'  5 s windows: {n}, mean {sum(fps) / n:.1f} fps, median {statistics.median(fps):.1f}, worst {min(fps):.0f}, '
                  f'best {max(fps):.0f}; below 144: {below(144):.0f}%, below 120: {below(120):.0f}%, below 100: {below(100):.0f}%')
        print(f'  10 s windows by draws/frame (fps, renderer thread ms/frame):')
        for lo, hi, name in BUCKETS:
            b = [w for w in native if lo <= w[2] < hi]
            if b:
                print(f'    {name:>10}: {len(b):3d} windows, {sum(w[1] for w in b) / len(b):6.1f} fps (min {min(w[1] for w in b):5.1f}),'
                      f' {sum(w[2] for w in b) / len(b):5.0f} draws/frame, renderer {sum(w[3] for w in b) / len(b):.1f} ms')
        if show:
            for fr, f, d, r in native:
                print(f'    swap {fr}: {f:6.1f} fps, {d:5.0f} draws/frame, renderer {r:.1f} ms')


if __name__ == '__main__':
    main()
