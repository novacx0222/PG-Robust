#!/usr/bin/env python3
"""
Generate a directory of synthetic error profiles.

    profiles.py KIND OUTDIR [--aliases t1,...] [--points 200] [--seed 7]

One file per base-relation alias ("<alias>.txt") and per pair of aliases
("<a>=<b>.txt", names in sorted order).  Each line is "sel_true sel_est".
The error of a point is x = log(sel_true / sel_est).

KIND:
  trivial          sel_true == sel_est, so x = 0 exactly; sel_est spread
                   log-uniformly over [1e-7, 1] so every bin has points
  ones             every line is "1 1" (x = 0, all points in one bin)
  shift:K          x = log K for every point (sel_est limited to <= 1/K so
                   that sel_true never needs clamping)
  twopoint:A,B     x is log A or log B, alternating (sel_est <= 1/max(A,B))
  lognormal:MU,SD  x ~ N(MU, SD), sel_true clamped to 1 (seeded)

trivial / ones must reproduce PostgreSQL exactly (with KDE bandwidth 0);
shift and twopoint have closed-form expected row counts.
"""
import argparse
import itertools
import math
import os
import random

DEFAULT_ALIASES = ['t%d' % i for i in range(1, 9)] + ['x', 'y', 'p', 'u']


def points(kind, n, rng):
    name, _, arg = kind.partition(':')
    out = []
    if name == 'ones':
        return [(1.0, 1.0)] * n
    if name == 'trivial':
        for i in range(n):
            e = 10 ** (-7 + 7 * (i + 0.5) / n)
            out.append((e, e))
        return out
    if name == 'shift':
        k = float(arg)
        hi = min(0.0, -math.log10(k)) if k > 1 else 0.0
        for i in range(n):
            e = 10 ** (-7 + (hi + 7) * (i + 0.5) / n)
            out.append((e * k, e))
        return out
    if name == 'twopoint':
        a, b = (float(v) for v in arg.split(','))
        hi = min(0.0, -math.log10(max(a, b))) if max(a, b) > 1 else 0.0
        for i in range(n):
            e = 10 ** (-7 + (hi + 7) * (i + 0.5) / n)
            out.append((e * (a if i % 2 == 0 else b), e))
        return out
    if name == 'lognormal':
        mu, sd = (float(v) for v in arg.split(','))
        for i in range(n):
            e = 10 ** rng.uniform(-7, 0)
            out.append((min(1.0, e * math.exp(rng.gauss(mu, sd))), e))
        return out
    raise SystemExit('unknown profile kind: ' + kind)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kind')
    ap.add_argument('outdir')
    ap.add_argument('--aliases', default=','.join(DEFAULT_ALIASES))
    ap.add_argument('--points', type=int, default=200)
    ap.add_argument('--seed', type=int, default=7)
    a = ap.parse_args()

    aliases = sorted(set(a.aliases.split(',')))
    keys = aliases + ['%s=%s' % (l, r) for l, r in itertools.combinations(aliases, 2)]
    os.makedirs(a.outdir, exist_ok=True)
    rng = random.Random(a.seed)
    for key in keys:
        with open(os.path.join(a.outdir, key + '.txt'), 'w') as f:
            for t, e in points(a.kind, a.points, rng):
                f.write('%.17g %.17g\n' % (t, e))
    print('%d profiles of kind %s in %s' % (len(keys), a.kind, a.outdir))


if __name__ == '__main__':
    main()
