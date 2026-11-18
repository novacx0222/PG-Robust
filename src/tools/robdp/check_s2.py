#!/usr/bin/env python3
"""
S2 gate: error profiles are parsed, binned and looked up exactly as
specified.  An independent Python implementation of the rules in
robustprofile.c is the reference.

    check_s2.py [--install v2-release]

Needs the test_robust module installed in the build (test_robust_profile).

  P1  bins: first, n, lo, hi equal; mean, sd, h bit-identical
      (all generated profiles x bin counts 1, 2, 3, 5, 8)
  P2  bin lookup of a grid of estimates equals the reference
  P3  edge cases: ties, too few points, sel_true = 0, CRLF, comments,
      invalid lines (counted in "skipped"), same points in another order
  P4  cache: no reload on reuse; reload when the file, the bin count or
      the directory changes
  P5  S1 gates still pass (planning unchanged)
"""
import argparse
import math
import os
import random
import shutil
import subprocess
import sys

import common
from robdp_dump import parse

MIN_SEL = 1e-12
WORK = os.path.join(common.ROOT, 'data', 'profiles', 's2test')


# ---------------------------------------------------------------- reference

def parse_file(path):
    pts, skipped = [], 0
    with open(path, newline='') as f:
        for raw in f.read().split('\n'):
            line = raw.lstrip(' \t')
            if line in ('', '\r') or line.startswith('#'):
                continue
            parts = line.split()
            try:
                if len(parts) != 2:
                    raise ValueError
                t, e = float(parts[0]), float(parts[1])
                if not (0.0 <= t <= 1.0 and 0.0 <= e <= 1.0):
                    raise ValueError
            except ValueError:
                skipped += 1
                continue
            pts.append((e, math.log(max(t, MIN_SEL) / max(e, MIN_SEL))))
    pts.sort()
    return pts, skipped


def llround(v):
    f = math.floor(v)
    return int(f + (1 if v - f >= 0.5 else 0))


def silverman(xs, sd):
    n = len(xs)
    if n < 2 or sd <= 0.0:
        return 0.0
    s = sorted(xs)
    iqr = s[(3 * (n - 1)) // 4] - s[(n - 1) // 4]
    spread = sd
    if iqr > 0.0 and iqr / 1.34 < sd:
        spread = iqr / 1.34
    return 0.9 * spread * math.pow(n, -0.2)


def make_bins(pts, B):
    N = len(pts)
    bins, start, prev_hi = [], 0, -math.inf
    for b in range(B):
        end = N if b == B - 1 else llround(((b + 1) * N) / B)
        end = max(min(end, N), start)
        while end > start and end < N and pts[end][0] == pts[end - 1][0]:
            end += 1
        xs = [x for _, x in pts[start:end]]
        n = len(xs)
        mean = sd = h = 0.0
        if n:
            acc = 0.0
            for x in xs:              # plain left-to-right sum, like the C loop
                acc += x
            mean = acc / n
            ss = 0.0
            for x in xs:
                ss += (x - mean) * (x - mean)
            sd = math.sqrt(ss / (n - 1)) if n > 1 else 0.0
            h = silverman(xs, sd)
        hi = pts[end - 1][0] if n else prev_hi
        bins.append({'first': start, 'n': n, 'lo': prev_hi, 'hi': hi, 'mean': mean, 'sd': sd, 'h': h})
        prev_hi, start = hi, end
    return bins


def find_bin(bins, s):
    for b, bin in enumerate(bins):
        if bin['n'] > 0 and s <= bin['hi']:
            return b
    for b in range(len(bins) - 1, -1, -1):
        if bins[b]['n'] > 0:
            return b
    return -1


def grid(pts):
    es = sorted({e for e, _ in pts})
    g = {0.0, 1.0, 1e-15, es[0] / 2, min(1.0, es[-1] * 1.5)}
    for a, b in zip(es, es[1:]):
        g.add((a + b) / 2)
    for e in es:
        g.update({e, math.nextafter(e, 0), math.nextafter(e, 2)})
    return sorted(v for v in g if 0.0 <= v <= 1.0)


# ---------------------------------------------------------------- fixtures

def write(path, lines, crlf=False):
    with open(path, 'w', newline='') as f:
        f.write(('\r\n' if crlf else '\n').join(lines) + '\n')


def make_fixtures():
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    subprocess.run([sys.executable, os.path.join(common.TOOLS, 'profiles.py'), 'lognormal:0.5,1.5',
                    WORK, '--aliases', 't1,t2,t3,t4,t5', '--points', '257'], check=True,
                   capture_output=True)
    rng = random.Random(11)
    pts = ['%.17g %.17g' % (min(1, e * math.exp(rng.gauss(0, 2))), e)
           for e in (10 ** rng.uniform(-6, 0) for _ in range(500))]
    write(os.path.join(WORK, 'e_shuffled_a.txt'), pts)
    rng.shuffle(pts)
    write(os.path.join(WORK, 'e_shuffled_b.txt'), pts)
    write(os.path.join(WORK, 'e_ones.txt'), ['1 1'] * 50)
    write(os.path.join(WORK, 'e_ties.txt'),
          ['%g %g' % (min(1, 0.01 * (i % 7 + 1)), [0.01, 0.01, 0.01, 0.02, 0.5][i % 5]) for i in range(103)])
    write(os.path.join(WORK, 'e_one_point.txt'), ['0.3 0.1'])
    write(os.path.join(WORK, 'e_three_points.txt'), ['0.3 0.1', '0.1 0.2', '0.9 0.3'])
    write(os.path.join(WORK, 'e_zero_true.txt'), ['0 0.5', '0 0.001', '0.5 0.5', '0 0'])
    write(os.path.join(WORK, 'e_crlf.txt'), ['# header', '0.2 0.1', '', '  0.4 0.3  ', '0.1 0.3'], crlf=True)
    write(os.path.join(WORK, 'e_invalid.txt'),
          ['0.2 0.1', 'x', '0.1', '0.1 0.2 0.3', '2 0.1', '0.1 -1', 'nan 0.1', '0.1 inf', '0.3 0.3'])
    write(os.path.join(WORK, 'e_big.txt'),
          ['%.17g %.17g' % (rng.random(), rng.random()) for _ in range(20000)])
    return sorted(f[:-4] for f in os.listdir(WORK) if f.endswith('.txt'))


# ---------------------------------------------------------------- driver

def run_session(install, sql_body, path=WORK, nbins=1):
    sql = ("SET error_profile_path = '%s';\nSET error_bin_count = %d;\n"
           "SET robust_debug_dump = on;\nSET client_min_messages = log;\n%s" % (path, nbins, sql_body))
    return common.psql(sql, install, on_error_stop=False)


def array_literal(vals):
    return "'{%s}'" % ','.join('%.17g' % v for v in vals)


def parse_int_array(s):
    s = s.strip()
    if not s:
        return None
    return [int(v) for v in s.strip('{}').split(',')] if s != '{}' else []


def report(tag, ok, detail=''):
    print('%-4s %s  %s' % (tag, 'PASS' if ok else 'FAIL', detail))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    a = ap.parse_args()
    common.start(a.install)
    common.psql('DROP EXTENSION IF EXISTS test_robust; CREATE EXTENSION test_robust;', a.install)
    keys = make_fixtures()
    allok = True

    # P1, P2, P3
    bin_mismatch, lookup_mismatch, n_lookups, n_bins = [], [], 0, 0
    skipped_mismatch = []
    for B in (1, 2, 3, 5, 8):
        ref = {k: parse_file(os.path.join(WORK, k + '.txt')) for k in keys}
        grids = {k: grid(ref[k][0]) for k in keys}
        body = ''.join('SELECT test_robust_profile(%s, %s);\n' % ("'%s'" % k, array_literal(grids[k]))
                       for k in keys)
        out, err = run_session(a.install, body, nbins=B)
        results = [l for l in out.split('\n')][:len(keys)]
        events = {e['key']: e for e in parse(err.splitlines()).get('profile', [])}
        for k, res in zip(keys, results):
            pts, skipped = ref[k]
            want = make_bins(pts, B)
            got = events.get(k)
            if got is None:
                bin_mismatch.append('%s B=%d: no profile event' % (k, B))
                continue
            if got['n'] != len(pts) or got['skipped'] != skipped:
                skipped_mismatch.append('%s: n %d/%d skipped %d/%d' % (k, got['n'], len(pts),
                                                                       got['skipped'], skipped))
            for b, (gb, wb) in enumerate(zip(got['bins'], want)):
                n_bins += 1
                for f in ('first', 'n', 'lo', 'hi', 'mean', 'sd', 'h'):
                    if gb[f] != wb[f]:
                        bin_mismatch.append('%s B=%d bin %d %s: C %r, Python %r' % (k, B, b, f, gb[f], wb[f]))
            got_bins = parse_int_array(res)
            want_bins = [find_bin(want, s) for s in grids[k]]
            n_lookups += len(want_bins)
            if got_bins != want_bins:
                lookup_mismatch.append('%s B=%d' % (k, B))
    allok &= report('P1', not bin_mismatch and not skipped_mismatch,
                    '%d profiles x 5 bin counts, %d bins: all fields identical' % (len(keys), n_bins)
                    if not bin_mismatch and not skipped_mismatch
                    else '; '.join((bin_mismatch + skipped_mismatch)[:5]))
    allok &= report('P2', not lookup_mismatch, '%d lookups identical' % n_lookups
                    if not lookup_mismatch else ', '.join(lookup_mismatch[:5]))

    # P3: spot checks of the edge cases (the exact values are covered by P1)
    out, err = run_session(a.install, ''.join("SELECT test_robust_profile('%s', '{0.1}');\n" % k
                                              for k in ('e_shuffled_a', 'e_shuffled_b', 'e_ones', 'e_ties',
                                                        'e_invalid', 'e_one_point')), nbins=8)
    ev = {e['key']: e for e in parse(err.splitlines()).get('profile', [])}
    warn_lines = [l for l in err.splitlines() if 'skipping invalid line' in l]
    checks = {
        'same points in another order give identical bins':
            ev['e_shuffled_a']['bins'] == ev['e_shuffled_b']['bins'],
        'all-equal estimates stay in one bin': [b['n'] for b in ev['e_ones']['bins']] == [50] + [0] * 7,
        'no bin boundary splits equal estimates':
            all(b['n'] == 0 or b['lo'] < b['hi'] or b['first'] == 0 for b in ev['e_ties']['bins']),
        '7 invalid lines: 5 warned, 2 valid points kept':
            ev['e_invalid']['skipped'] == 7 and ev['e_invalid']['n'] == 2 and len(warn_lines) == 5,
        'one point, 8 bins: exactly one non-empty bin':
            sorted(b['n'] for b in ev['e_one_point']['bins']) == [0] * 7 + [1],
    }
    bad = [k for k, ok in checks.items() if not ok]
    allok &= report('P3', not bad, '%d edge-case checks' % len(checks) if not bad else '; '.join(bad))

    # P4: cache behaviour within one session
    other = WORK + '_other'
    if os.path.isdir(other):
        shutil.rmtree(other)
    os.makedirs(other)
    write(os.path.join(other, 'e_one_point.txt'), ['0.3 0.1', '0.6 0.2'])
    mod = os.path.join(WORK, 'e_three_points.txt')
    body = ("SELECT test_robust_profile('e_three_points', '{0.1}');\n"
            "SELECT test_robust_profile('e_three_points', '{0.1}');\n"            # cached
            "\\! printf '0.3 0.1\\n0.1 0.2\\n0.9 0.3\\n0.5 0.4\\n' > %s\n"         # file changes
            "SELECT test_robust_profile('e_three_points', '{0.1}');\n"
            "SET error_bin_count = 2;\n"
            "SELECT test_robust_profile('e_three_points', '{0.1}');\n"            # bins change
            "SELECT test_robust_profile('e_one_point', '{0.1}');\n"
            "SET error_profile_path = '%s';\n"
            "SELECT test_robust_profile('e_one_point', '{0.1}');\n"               # directory changes
            % (mod, other))
    out, err = run_session(a.install, body, nbins=1)
    loads = [(e['key'], e['n'], len(e['bins'])) for e in parse(err.splitlines()).get('profile', [])]
    want = [('e_three_points', 3, 1), ('e_three_points', 4, 1), ('e_three_points', 4, 2),
            ('e_one_point', 1, 2), ('e_one_point', 2, 2)]
    allok &= report('P4', loads == want, 'loads %s' % loads if loads != want else
                    'reuse without reload; reload on file, bin count and directory change')

    # P5
    r = subprocess.run([sys.executable, os.path.join(common.TOOLS, 'check_s1.py'), '--install', a.install],
                       capture_output=True, text=True)
    allok &= report('P5', r.returncode == 0, 'check_s1: ' + ' | '.join(
        l.split('  ', 1)[0] + ' ' + l.split()[1] for l in r.stdout.splitlines() if l[:1] == 'G'))

    common.stop()
    sys.exit(0 if allok else 1)


if __name__ == '__main__':
    main()
