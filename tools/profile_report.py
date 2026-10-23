#!/usr/bin/env python3
"""Aggregate HC09_PROFILE counts (runtime/profiler.cpp, default captures/profile.txt) and name the
functions. Run from the repo root.

  py -3.11 tools/profile_report.py captures/profile.txt                     all windows
  py -3.11 tools/profile_report.py captures/profile.txt --swaps 19000-40000[,a-b] windows inside swap ranges
  py -3.11 tools/profile_report.py captures/profile.txt --windows 5-20,31,40-42 --thread render --top 40
  options: --no-hints (skip the static index), --callers N (callers shown per function, default 2)

Per thread (merged by tid): ms per frame (wall clock: share of samples x window time / frames),
categories, then the top functions by self and by inclusive samples. Names: imports from
out/ppc_func_mapping.cpp, D3D functions from docs/D3D_API.md, otherwise hints from tools/ppcxref.py
(strings the function or its caller references) and the function's size in guest instructions.
Keys: sub_X guest function; hook:sub_X the runtime's wrapper of it (self only); imp:X an import;
runtime<K runtime code under K (the native front end under D3D hooks, imports' implementation);
[wait:]module!export other modules (wait: = in a system call; the nearest export, so ntdll
internals show under a neighbouring export name).
"""
import argparse
import collections
import os
import pickle
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))


def parse(path):
    windows, cur, th = [], None, None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('#') or not line:
            continue
        if line.startswith('window '):
            p = line.split()
            cur = {'idx': int(p[1]), 'sec': float(p[3]), 'sw0': int(p[5]), 'sw1': int(p[6]), 'threads': {}}
            windows.append(cur)
            continue
        if line.startswith('thread '):
            m = re.match(r'thread (\d+) (.*) samples (\d+) suspended_us (\S+)$', line)
            tid, name = int(m.group(1)), m.group(2)
            key = tid if tid else name
            th = cur['threads'].setdefault(key, {'name': name, 'samples': 0, 'susp': 0.0, 'cat': collections.Counter(),
                                                 'self': collections.Counter(), 'incl': collections.Counter(),
                                                 'inclg': collections.Counter(),
                                                 'edge': collections.defaultdict(collections.Counter)})
            th['samples'] += int(m.group(3))
            th['susp'] += float(m.group(4))
            continue
        kind, rest = line.split(' ', 1)
        if kind == 'cat':
            name, n = rest.rsplit('|', 1)
            th['cat'][name] += int(n)
        elif kind in ('self', 'incl', 'inclg'):
            k, n = rest.rsplit(' ', 1)
            th[kind][k] += int(n)
        elif kind == 'edge':
            a, b, n = rest.split(' ')
            th['edge'][a][b] += int(n)
    return windows


def import_names():
    names = {}
    path = os.path.join(ROOT, 'out', 'ppc_func_mapping.cpp')
    if os.path.exists(path):
        for m in re.finditer(r'\{ 0x([0-9A-F]{8}), __imp__(\w+) \}', open(path, encoding='utf-8', errors='replace').read()):
            names[int(m.group(1), 16)] = m.group(2)
    return names


def d3d_names():
    names = {}
    path = os.path.join(ROOT, 'docs', 'D3D_API.md')
    if os.path.exists(path):
        for m in re.finditer(r'^#+ sub_([0-9A-F]{8}) [^\w]+ (\w+)', open(path, encoding='utf-8', errors='replace').read(), re.M):
            names[int(m.group(1), 16)] = m.group(2)
    return names


def sizes():
    """Guest instructions per function (cached in build-run/ppcsize.pkl)."""
    import ppcxref
    cache = os.path.join(ROOT, 'build-run', 'ppcsize.pkl')
    newest = max(os.path.getmtime(f) for f in ppcxref.sources())
    if os.path.exists(cache) and os.path.getmtime(cache) >= newest:
        return pickle.load(open(cache, 'rb'))
    print('(counting function sizes, ~1 min)', file=sys.stderr)
    out, cur = {}, None
    for fn in ppcxref.sources():
        for line in open(fn, encoding='utf-8', errors='replace'):
            m = ppcxref.FRE.search(line)
            if m:
                cur = int(m.group(1), 16)
                out[cur] = 0
            elif cur is not None and line.startswith('\t// '):
                out[cur] += 1
    pickle.dump(out, open(cache, 'wb'))
    return out


class Namer:
    def __init__(self, hints):
        self.imp = import_names()
        self.d3d = d3d_names()
        self.hints = hints
        self.idx = None
        self.size = {}
        if hints:
            import ppcxref
            self.x = ppcxref
            self.idx = ppcxref.index()
            self.size = sizes()
            self.fstrs = collections.defaultdict(list)
            refs = self.idx[0]
            for a, lst in refs.items():
                if ppcxref.BASE <= a < 0x82300000:
                    for f, _ in lst:
                        if len(self.fstrs[f]) < 6:
                            self.fstrs[f].append(a)

    def strs(self, f):
        if not self.idx:
            return []
        out = []
        for a in self.fstrs.get(f, []):
            s = self.x.cstr(a)
            if s and s not in out:
                out.append(s)
        return out[:3]

    def label(self, key):
        m = re.match(r'(hook:)?sub_([0-9A-F]{8})$', key)
        if m:
            return key + self.describe(int(m.group(2), 16))
        m = re.match(r'imp:([0-9A-F]{8})$', key)
        if m:
            return 'imp:' + self.imp.get(int(m.group(1), 16), m.group(1))
        m = re.match(r'runtime<(imp:|hook:sub_|sub_)([0-9A-F]{8})$', key)
        if m:
            a = int(m.group(2), 16)
            inner = 'imp:' + self.imp.get(a, m.group(2)) if m.group(1) == 'imp:' else m.group(1) + m.group(2) + self.describe(a, short=True)
            return 'runtime<' + inner
        return key

    def describe(self, f, short=False):
        if f in self.d3d:
            return ' [D3D %s]' % self.d3d[f]
        if short or not self.hints:
            return ''
        bits = []
        if f in self.size:
            bits.append('%d insns' % self.size[f])
        s = self.strs(f)
        if s:
            bits.append(' | '.join(repr(x)[1:-1][:40] for x in s))
        return ' [' + '; '.join(bits) + ']' if bits else ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--swaps')
    ap.add_argument('--windows')
    ap.add_argument('--thread')
    ap.add_argument('--top', type=int, default=30)
    ap.add_argument('--callers', type=int, default=2)
    ap.add_argument('--no-hints', action='store_true')
    ap.add_argument('--by-game', action='store_true', help='rank inclusive by game-code samples')
    a = ap.parse_args()
    wins = parse(a.file)
    if a.swaps:   # a-b[,c-d...]
        spans = [tuple(map(int, part.split('-'))) for part in a.swaps.split(',')]
        wins = [w for w in wins if any(w['sw0'] >= lo and w['sw1'] <= hi for lo, hi in spans)]
    if a.windows:
        keep = set()
        for part in a.windows.split(','):
            lo, _, hi = part.partition('-')
            keep.update(range(int(lo), int(hi or lo) + 1))
        wins = [w for w in wins if w['idx'] in keep]
    if not wins:
        print('no windows selected'); return
    sec = sum(w['sec'] for w in wins)
    frames = sum(w['sw1'] - w['sw0'] for w in wins)
    print('%d windows (%d-%d), %.0f s, swaps %d-%d, %d frames, %.1f frames/s, %.2f ms/frame'
          % (len(wins), wins[0]['idx'], wins[-1]['idx'], sec, wins[0]['sw0'], wins[-1]['sw1'], frames, frames / sec,
             sec * 1000 / max(frames, 1)))
    namer = Namer(not a.no_hints)
    merged = {}
    for w in wins:
        for key, t in w['threads'].items():
            m = merged.setdefault(key, {'name': t['name'], 'samples': 0, 'susp': 0.0, 'sec': 0.0, 'frames': 0,
                                        'cat': collections.Counter(), 'self': collections.Counter(),
                                        'incl': collections.Counter(), 'inclg': collections.Counter(),
                                        'edge': collections.defaultdict(collections.Counter)})
            m['name'] = t['name']
            m['samples'] += t['samples']; m['susp'] += t['susp']
            m['sec'] += w['sec']; m['frames'] += w['sw1'] - w['sw0']
            for k in ('cat', 'self', 'incl', 'inclg'):
                m[k].update(t[k])
            for x, c in t['edge'].items():
                m['edge'][x].update(c)
    for key, t in merged.items():
        if a.thread and a.thread.lower() not in t['name'].lower():
            continue
        n = t['samples']
        mpf = t['sec'] * 1000 / max(t['frames'], 1)
        ms = lambda c: c / n * mpf
        print('\n== %s (tid %s): %d samples, %.2f ms/frame, suspended by the profiler %.2f%% of the time'
              % (t['name'], key, n, mpf, t['susp'] / 1e4 / t['sec']))
        for c, v in t['cat'].most_common():
            print('   %6.2f ms  %5.1f%%  %s' % (ms(v), 100 * v / n, c))

        def callers(k):
            e = t['edge'].get(k.replace('hook:', ''))
            if not e:
                return ''
            tot = sum(e.values())
            return '  <- ' + ', '.join('%s %.0f%%' % (namer.label(c).split(' [')[0], 100 * v / tot)
                                       for c, v in e.most_common(a.callers))
        print('  top self:')
        for k, v in t['self'].most_common(a.top):
            print('   %6.3f ms  %5.2f%%  %s%s' % (ms(v), 100 * v / n, namer.label(k), callers(k)))
        print('  top inclusive (below 97%%; "game" = only samples in the game\'s own code, not D3D/front end/OS):')
        shown = 0
        order = sorted(t['incl'], key=lambda k: -(t['inclg'][k] if a.by_game else t['incl'][k]))
        for k in order:
            v = t['incl'][k]
            if v > 0.97 * n:
                continue
            g = ' game %6.3f ms' % ms(t['inclg'][k]) if t['inclg'] else ''
            print('   %6.3f ms  %5.2f%%%s  self %5.2f%%  %s%s' % (ms(v), 100 * v / n, g,
                                                           100 * (t['self'].get(k, 0) + t['self'].get('hook:' + k, 0)) / n,
                                                           namer.label(k), callers(k)))
            shown += 1
            if shown >= a.top:
                break


if __name__ == '__main__':
    main()
