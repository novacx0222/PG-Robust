"""enable_bitmapscan=off vs on, max parallel, stock and E[Pen]x8 (pen8):
ep8.py output with ROBDP_PAR_GUCS=...,enable_bitmapscan=off (CONFIGS=pen8)
against the bitmap-on run with the same settings, per system.

    python3 nobm_report.py OFF_V2.json ON_V2.json OFF_V20.json ON_V20.json
"""
import collections, json, math, statistics, sys


def gm(x):
    return math.exp(sum(math.log(v) for v in x) / len(x)) if x else float('nan')


systems = [('v2', json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))),
           ('v20', json.load(open(sys.argv[3])), json.load(open(sys.argv[4])))]
for name, off, on in systems:
    errs = {n: [t for t in ('pg', 'pen8') if 'ms' not in off[n].get(t, {})] for n in off}
    errs = {n: t for n, t in errs.items() if t}
    ns = [n for n in off if n in on and n not in errs]
    print('== %s: %d queries%s' % (name, len(ns), ('; failed (timeout?) %s' % errs) if errs else ''))
    print('%-14s %9s %9s %8s %9s %8s %9s' % ('', 'exec sum', 'sum-T23', 'Bitmap', 'plan med', 'gm vs', 'gm vs'))
    print('%-14s %9s %9s %8s %9s %8s %9s' % ('', 's', 's', 'plans', 'ms', 'stock', 'stock on'))
    for lab, d, t in (('stock on', on, 'pg'), ('stock off', off, 'pg'), ('pen8 on', on, 'pen8'), ('pen8 off', off, 'pen8')):
        print('%-14s %9.1f %9.1f %8d %9.1f %8s %9.3f' % (
            lab, sum(d[n][t]['ms'] for n in ns) / 1000,
            sum(d[n][t]['ms'] for n in ns if not n.startswith('23-')) / 1000,
            sum('Bitmap' in d[n][t]['shape'] for n in ns), statistics.median(d[n][t]['plan_ms'] for n in ns),
            '' if t == 'pg' else '%.3f' % gm([d[n]['pg']['ms'] / d[n][t]['ms'] for n in ns]),
            gm([on[n]['pg']['ms'] / d[n][t]['ms'] for n in ns])))
    print('pen8 vs stock, same bitmap setting, excluding T23: on %.3f, off %.3f' % tuple(
        gm([d[n]['pg']['ms'] / d[n]['pen8']['ms'] for n in ns if not n.startswith('23-')]) for d in (on, off)))
    for t in ('pg', 'pen8'):
        same = sum(off[n][t]['shape'] == on[n][t]['shape'] for n in ns)
        chg = [n for n in ns if off[n][t]['shape'] != on[n][t]['shape']]
        print('%s: plan unchanged by bitmap off %d/%d; changed plans on/off exec geomean %.3f' % (
            t, same, len(ns), gm([on[n][t]['ms'] / off[n][t]['ms'] for n in chg])))
    print('pen8 >10%% faster / slower than stock: on %d/%d, off %d/%d' % (
        sum(on[n]['pg']['ms'] / on[n]['pen8']['ms'] > 1.1 for n in ns),
        sum(on[n]['pg']['ms'] / on[n]['pen8']['ms'] < 1 / 1.1 for n in ns),
        sum(off[n]['pg']['ms'] / off[n]['pen8']['ms'] > 1.1 for n in ns),
        sum(off[n]['pg']['ms'] / off[n]['pen8']['ms'] < 1 / 1.1 for n in ns)))
    bt = collections.defaultdict(list)
    for n in ns:
        bt[n.split('-')[0]].append(n)
    print('%-4s %9s %9s %9s %9s %11s %11s' % ('T', 'stock on', 'stock off', 'pen8 on', 'pen8 off',
                                             'pen8/st on', 'pen8/st off'))
    for k in sorted(bt, key=int):
        q = bt[k]
        print('%-4s %9.2f %9.2f %9.2f %9.2f %11.2f %11.2f' % (
            k, *(sum(d[n][t]['ms'] for n in q) / 1000 for d, t in ((on, 'pg'), (off, 'pg'), (on, 'pen8'), (off, 'pen8'))),
            gm([on[n]['pg']['ms'] / on[n]['pen8']['ms'] for n in q]),
            gm([off[n]['pg']['ms'] / off[n]['pen8']['ms'] for n in q])))
a, b = systems[0][1], systems[1][1]
common = [n for n in a if n in b]
print('stock plans equal in the v2 / v20 runs (bitmap off): %d/%d' % (
    sum(a[n]['pg'].get('shape') == b[n]['pg'].get('shape') for n in common), len(common)))
print('pen8 plans equal v2 / v20 (bitmap off): %d/%d' % (
    sum(a[n]['pen8'].get('shape') == b[n]['pen8'].get('shape') for n in common), len(common)))
