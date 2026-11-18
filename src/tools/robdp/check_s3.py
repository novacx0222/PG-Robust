#!/usr/bin/env python3
"""
S3 gate: errors are drawn from the profiles exactly as specified in
robustsample.c.  An independent Python implementation of pg_prng,
hash_bytes_extended and the drawing rules is the reference.

    check_s3.py [--install v2-release]

Needs the test_robust module installed in the build (test_robust_draw).

  D1  seeds: the "seed" of every draw equals the reference (instance keys
      of length 0..40, UTF-8, quotes), for 3 values of error_sample_seed
  D2  draws bit-identical to the reference: profiles x bin counts x
      bandwidths (0, fixed, Silverman) x N (1, 20, 1000) x estimates
  D3  bandwidth 0: every x[s] is one of the bin's log ratios
  D4  properties: reproducible across sessions; N=10 is a prefix of
      N=1000; the bandwidth does not change the points drawn; estimates in
      different bins of one instance use the same indices
  D5  statistics (N = 1000, fixed seeds): the two instances of a self-join
      are uncorrelated; bootstrap draws follow the bin (KS test); KDE draws
      have mean ~ bin mean and variance ~ var + h^2
  D6  S2 gates (and through them the S1 gates) still pass
"""
import argparse
import math
import os
import subprocess
import sys

import common
import check_s2
from check_s2 import WORK, parse_file, make_bins, find_bin, grid
from robdp_dump import parse

M64 = (1 << 64) - 1
M32 = (1 << 32) - 1
NOISE_STREAM = 0x6e6f697365


# ---------------------------------------------------------------- reference

def rot32(x, k):
    return ((x << k) | (x >> (32 - k))) & M32


def _mix(a, b, c):
    a = (a - c) & M32; a ^= rot32(c, 4);  c = (c + b) & M32
    b = (b - a) & M32; b ^= rot32(a, 6);  a = (a + c) & M32
    c = (c - b) & M32; c ^= rot32(b, 8);  b = (b + a) & M32
    a = (a - c) & M32; a ^= rot32(c, 16); c = (c + b) & M32
    b = (b - a) & M32; b ^= rot32(a, 19); a = (a + c) & M32
    c = (c - b) & M32; c ^= rot32(b, 4);  b = (b + a) & M32
    return a, b, c


def _final(a, b, c):
    c ^= b; c = (c - rot32(b, 14)) & M32
    a ^= c; a = (a - rot32(c, 11)) & M32
    b ^= a; b = (b - rot32(a, 25)) & M32
    c ^= b; c = (c - rot32(b, 16)) & M32
    a ^= c; a = (a - rot32(c, 4)) & M32
    b ^= a; b = (b - rot32(a, 14)) & M32
    c ^= b; c = (c - rot32(b, 24)) & M32
    return a, b, c


def hash_bytes_extended(k, seed):
    """src/common/hashfn.c, little-endian"""
    n = len(k)
    a = b = c = (0x9e3779b9 + n + 3923095) & M32
    if seed != 0:
        a = (a + (seed >> 32)) & M32
        b = (b + (seed & M32)) & M32
        a, b, c = _mix(a, b, c)
    i = 0
    while n - i >= 12:
        a = (a + int.from_bytes(k[i:i + 4], 'little')) & M32
        b = (b + int.from_bytes(k[i + 4:i + 8], 'little')) & M32
        c = (c + int.from_bytes(k[i + 8:i + 12], 'little')) & M32
        a, b, c = _mix(a, b, c)
        i += 12
    t = k[i:]
    a = (a + int.from_bytes(t[0:4], 'little')) & M32
    b = (b + int.from_bytes(t[4:8], 'little')) & M32
    c = (c + (int.from_bytes(t[8:11], 'little') << 8)) & M32   # low byte of c is reserved
    a, b, c = _final(a, b, c)
    return (b << 32) | c


def hash_combine64(a, b):
    return a ^ ((b + 0x49a0f4dd15e5a8e3 + (a << 54) + (a >> 7)) & M64)


def draw_seed(key, inst, seed_guc):
    s = hash_bytes_extended(key.encode(), seed_guc)
    return hash_combine64(s, hash_bytes_extended(inst.encode(), 0))


class Prng:
    """src/common/pg_prng.c"""

    def __init__(self, seed):
        st = [seed & M64]

        def splitmix():
            st[0] = (st[0] + 0x9E3779B97F4A7C15) & M64
            v = st[0]
            v = ((v ^ (v >> 30)) * 0xBF58476D1CE4E5B9) & M64
            v = ((v ^ (v >> 27)) * 0x94D049BB133111EB) & M64
            return v ^ (v >> 31)
        self.s0 = splitmix()
        self.s1 = splitmix()
        if self.s0 == 0 and self.s1 == 0:
            self.s0, self.s1 = 0x5851F42D4C957F2D, 0x14057B7EF767814F

    @staticmethod
    def _rotl(x, k):
        return ((x << k) | (x >> (64 - k))) & M64

    def next(self):
        s0 = self.s0
        sx = self.s1 ^ s0
        val = (self._rotl((s0 * 5) & M64, 7) * 9) & M64
        self.s0 = self._rotl(s0, 24) ^ sx ^ ((sx << 16) & M64)
        self.s1 = self._rotl(sx, 37)
        return val

    def uint64_range(self, rmin, rmax):
        if rmax <= rmin:
            return rmin
        rng = rmax - rmin
        rshift = 63 - (rng.bit_length() - 1)
        while True:
            v = self.next() >> rshift
            if v <= rng:
                return rmin + v

    def double(self):
        return math.ldexp(float(self.next() >> 12), -52)

    def normal(self):
        u1 = 1.0 - self.double()
        u2 = 1.0 - self.double()
        return math.sqrt(-2.0 * math.log(u1)) * math.sin(2.0 * math.pi * u2)


def bandwidth(bins, b, bw_guc):
    if bw_guc > 0:
        return bw_guc
    if bw_guc < 0:
        return bins[b]['h']
    return 0.0


def draw(pts, bins, key, inst, sel_est, n, seed_guc, bw_guc):
    """returns (seed, bin, h, x, indices)"""
    b = find_bin(bins, sel_est)
    seed = draw_seed(key, inst, seed_guc)
    rng, noise = Prng(seed), Prng(hash_combine64(seed, NOISE_STREAM))
    h = bandwidth(bins, b, bw_guc)
    first, cnt = bins[b]['first'], bins[b]['n']
    xs, idxs = [], []
    for _ in range(n):
        i = first + rng.uint64_range(0, cnt - 1)
        v = pts[i][1]
        if h > 0.0:
            v = v + h * noise.normal()
        xs.append(v)
        idxs.append(i)
    return seed, b, h, xs, idxs


# ---------------------------------------------------------------- driver

def q(s):
    return "'" + s.replace("'", "''") + "'"


def session(install, calls, n, seed_guc, bw, nbins):
    """calls: list of (key, inst, sel_est) -> list of draw events, in order"""
    sql = ("SET error_profile_path = %s;\nSET error_bin_count = %d;\nSET error_sample_count = %d;\n"
           "SET error_sample_seed = %d;\nSET error_sample_kde_bandwidth = %.17g;\n"
           "SET robust_debug_dump = on;\nSET client_min_messages = log;\n" % (q(WORK), nbins, n, seed_guc, bw))
    sql += ''.join('SELECT test_robust_draw(%s, %s, %.17g) IS NULL;\n' % (q(k), q(i), s) for k, i, s in calls)
    out, err = common.psql(sql, install, on_error_stop=False)
    ev = parse(err.splitlines()).get('draw', [])
    if len(ev) != len(calls):
        raise SystemExit('expected %d draw events, got %d:\n%s' % (len(calls), len(ev), err[-2000:]))
    return ev


def report(tag, ok, detail=''):
    print('%-4s %s  %s' % (tag, 'PASS' if ok else 'FAIL', detail))
    return ok


INSTS = ['', 'x', 'y', 't1', 'x=y', 'abc', 'abcd', 'abcdefg', 'abcdefgh', 'abcdefghijk', 'abcdefghijkl',
         'abcdefghijklm', 'a' * 23, 'b' * 24, 'c' * 25, 'd' * 40, 'é', 'tëst=ünïcode', "o'brien", 'T1']


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    a = ap.parse_args()
    common.start(a.install)
    common.psql('DROP EXTENSION IF EXISTS test_robust; CREATE EXTENSION test_robust;', a.install)
    check_s2.make_fixtures()
    keys = ['t1', 't2', 't1=t2', 'e_ties', 'e_one_point', 'e_zero_true', 'e_three_points', 'e_big']
    raw = {k: parse_file(os.path.join(WORK, k + '.txt'))[0] for k in keys}
    allok = True

    # D1: seeds
    bad, n_seeds = [], 0
    for sg in (0, 42, 2147483647):
        calls = [(k, i, 0.1) for k in ('t1', 't1=t2') for i in INSTS]
        for (k, i, _), e in zip(calls, session(a.install, calls, 1, sg, 0.0, 1)):
            n_seeds += 1
            want = '%016x' % draw_seed(k, i, sg)
            if e['seed'] != want or e['inst'] != i:
                bad.append('%s/%r seed=%d: C %s, Python %s' % (k, i, sg, e['seed'], want))
    allok &= report('D1', not bad, '%d seeds identical (instance lengths 0..40, UTF-8, quotes)' % n_seeds
                    if not bad else '; '.join(bad[:4]))

    # D2, D3
    configs = []
    for B in (1, 3, 8):
        for bw in (0.0, 0.25, -1.0):
            for sg in (0, 42, 2147483647):
                configs.append((B, bw, sg, 20))
    configs += [(1, 0.0, 42, 1), (3, -1.0, 42, 1), (2, 0.0, 42, 1000), (2, -1.0, 42, 1000),
                (5, 0.3, 7, 1000)]
    mismatches, not_point, n_draws, n_values = [], [], 0, 0
    for B, bw, sg, N in configs:
        bins = {k: make_bins(raw[k], B) for k in keys}
        calls = []
        for k in keys:
            g = grid(raw[k])
            sel = g if N <= 20 and len(g) <= 12 else [g[j * (len(g) - 1) // 5] for j in range(6)]
            insts = ['x', 'y'] if N == 1000 else ['x', 'y', k, 'abcdefghijklm']
            calls += [(k, i, s) for i in insts for s in sel]
        for (k, i, s), e in zip(calls, session(a.install, calls, N, sg, bw, B)):
            seed, b, h, xs, idxs = draw(raw[k], bins[k], k, i, s, N, sg, bw)
            n_draws += 1
            n_values += N
            got = e['x'] if isinstance(e['x'], list) else [e['x']] * N
            if e['bin'] != b or e['h'] != h or got != xs:
                diff = next((j for j in range(N) if got[j] != xs[j]), None)
                mismatches.append('%s/%s B=%d bw=%g seed=%d N=%d sel=%r: bin %s/%s h %r/%r first diff at %s'
                                  % (k, i, B, bw, sg, N, s, e['bin'], b, e['h'], h, diff))
            if bw == 0.0:
                allowed = {x for _, x in raw[k][bins[k][b]['first']:bins[k][b]['first'] + bins[k][b]['n']]}
                if not set(got) <= allowed:
                    not_point.append('%s/%s B=%d sel=%r' % (k, i, B, s))
    allok &= report('D2', not mismatches, '%d draws (%d values) over %d configurations bit-identical'
                    % (n_draws, n_values, len(configs)) if not mismatches else '; '.join(mismatches[:3]))
    allok &= report('D3', not not_point, 'bandwidth 0: every value is a point of its bin'
                    if not not_point else '; '.join(not_point[:3]))

    # D4: properties, from C output only
    # e_big has 20000 distinct points: 4 bins of 5000, so estimates in the
    # first and the last bin draw indices from the same range
    bins_big = make_bins(raw['e_big'], 4)
    lo_s, hi_s = bins_big[0]['hi'], bins_big[3]['hi']
    calls = [('e_big', 'x', lo_s), ('e_big', 'x', hi_s), ('t1', 'y', 0.3)]
    r1 = session(a.install, calls, 1000, 42, 0.0, 4)
    r2 = session(a.install, calls, 1000, 42, 0.0, 4)
    r10 = session(a.install, calls, 10, 42, 0.0, 4)
    rbw = session(a.install, calls, 1000, 42, 1e-9, 4)
    idx = []
    for (k, i, s), e in zip(calls[:2], r1):
        bb = bins_big[e['bin']]
        pos = {raw[k][j][1]: j - bb['first'] for j in range(bb['first'], bb['first'] + bb['n'])}
        idx.append([pos[v] for v in e['x']])
    checks = {
        'reproducible across sessions': [e['x'] for e in r1] == [e['x'] for e in r2],
        'N=10 draws are a prefix of N=1000': all(e10['x'] == e['x'][:10] for e10, e in zip(r10, r1)),
        'bandwidth 1e-9 keeps the points (|diff| < 1e-7, none equal)':
            all(all(abs(u - v) < 1e-7 and u != v for u, v in zip(eb['x'], e['x'])) for eb, e in zip(rbw, r1)),
        'different bins of one instance draw the same indices':
            (r1[0]['bin'], r1[1]['bin']) == (0, 3) and
            len({x for _, x in raw['e_big']}) == len(raw['e_big']) and
            [b['n'] for b in bins_big] == [5000] * 4 and idx[0] == idx[1],
    }
    bad = [k for k, ok in checks.items() if not ok]
    allok &= report('D4', not bad, '%d property checks' % len(checks) if not bad else '; '.join(bad))

    # D5: statistics
    N = 1000
    calls = [('t1', 'x', 0.05), ('t1', 'y', 0.05), ('e_big', 'x', 0.5)]
    boot = session(a.install, calls, N, 42, 0.0, 2)
    kde = session(a.install, [('e_big', 'x', 0.5), ('t1', 'x', 0.05)], N, 42, -1.0, 2)
    fixed = session(a.install, [('e_big', 'x', 0.5)], N, 42, 0.5, 2)

    def corr(u, v):
        mu, mv = sum(u) / len(u), sum(v) / len(v)
        cu = sum((p - mu) * (r - mv) for p, r in zip(u, v))
        return cu / math.sqrt(sum((p - mu) ** 2 for p in u) * sum((r - mv) ** 2 for r in v))

    def ks(sample, pop):
        sample, pop = sorted(sample), sorted(pop)
        d, i, j = 0.0, 0, 0
        for v in sorted(set(sample) | set(pop)):
            while i < len(sample) and sample[i] <= v:
                i += 1
            while j < len(pop) and pop[j] <= v:
                j += 1
            d = max(d, abs(i / len(sample) - j / len(pop)))
        return d

    def bin_xs(k, e, B):
        bb = make_bins(raw[k], B)[e['bin']]
        return [x for _, x in raw[k][bb['first']:bb['first'] + bb['n']]], bb

    r = corr(boot[0]['x'], boot[1]['x'])
    stats = ['corr(x, y) = %+.3f' % r]
    ok5 = abs(r) < 4 / math.sqrt(N)
    for e, k in ((boot[0], 't1'), (boot[2], 'e_big')):
        pop, _ = bin_xs(k, e, 2)
        d = ks(e['x'], pop)
        stats.append('KS %s = %.3f' % (k, d))
        ok5 &= d < 1.63 / math.sqrt(N)
    for e, k, h_of in ((kde[0], 'e_big', None), (kde[1], 't1', None), (fixed[0], 'e_big', 0.5)):
        pop, bb = bin_xs(k, e, 2)
        h = bb['h'] if h_of is None else h_of
        m = sum(e['x']) / N
        var = sum((v - m) ** 2 for v in e['x']) / (N - 1)
        pm = sum(pop) / len(pop)
        pvar = sum((v - pm) ** 2 for v in pop) / len(pop)
        want_var = pvar + h * h
        z = (m - pm) / math.sqrt(want_var / N)
        rel = var / want_var
        stats.append('%s h=%.3f: mean z=%+.2f, var ratio %.3f' % (k, h, z, rel))
        ok5 &= abs(z) < 4 and abs(rel - 1) < 0.25
    allok &= report('D5', ok5, '; '.join(stats))

    # D6
    common.stop()
    r = subprocess.run([sys.executable, os.path.join(common.TOOLS, 'check_s2.py'), '--install', a.install],
                       capture_output=True, text=True)
    allok &= report('D6', r.returncode == 0, 'check_s2: ' + ' | '.join(
        l.split()[0] + ' ' + l.split()[1] for l in r.stdout.splitlines() if l[:1] == 'P'))
    common.stop()
    sys.exit(0 if allok else 1)


if __name__ == '__main__':
    main()
