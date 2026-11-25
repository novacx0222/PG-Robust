"""Max-parallel run (ep8.py with ROBDP_PAR=8 etc.) for v2 and v20 side by side:
per configuration exec geomean vs stock (also without T23), exec sum,
plan+exec geomean, planning median, Gather count, plans equal to K=1; then
per template.

    python3 pmax_report.py V2.json V20.json
"""
import json, math, statistics, sys, collections

runs = [('v2', json.load(open(sys.argv[1]))), ('v20', json.load(open(sys.argv[2])))]
TAGS = ('cost0', 'cost8', 'pen0', 'pen8')
BASE = {'cost8': 'cost0', 'pen8': 'pen0'}


def gm(x):
    return math.exp(sum(math.log(v) for v in x) / len(x)) if x else float('nan')


for sysname, d in runs:
    ok = {n: r for n, r in d.items() if all('ms' in r.get(t, {}) for t in ('pg',) + TAGS)}
    print('== %s: %d queries, stock exec sum %.1f s (no T23 %.1f s), stock planning median %.1f ms, Gather %d' % (
        sysname, len(ok), sum(r['pg']['ms'] for r in ok.values()) / 1000,
        sum(r['pg']['ms'] for n, r in ok.items() if not n.startswith('23-')) / 1000,
        statistics.median(r['pg']['plan_ms'] for r in ok.values()), sum('Gather' in r['pg']['shape'] for r in ok.values())))
    print('%-6s %7s %8s %8s %8s %9s %9s %7s %7s %7s' % ('cfg', 'gm', 'gm-T23', 'sum s', 'sum-T23', 'plan+exec', 'plan med',
                                                     'Gather', '>10%', '<10%'))
    for t in TAGS:
        rs = ok.values()
        print('%-6s %7.3f %8.3f %8.1f %8.1f %9.3f %9.1f %7d %7d %7d%s' % (
            t, gm([r['pg']['ms'] / r[t]['ms'] for r in rs]),
            gm([r['pg']['ms'] / r[t]['ms'] for n, r in ok.items() if not n.startswith('23-')]),
            sum(r[t]['ms'] for r in rs) / 1000,
            sum(r[t]['ms'] for n, r in ok.items() if not n.startswith('23-')) / 1000,
            gm([(r['pg']['ms'] + r['pg']['plan_ms']) / (r[t]['ms'] + r[t]['plan_ms']) for r in rs]),
            statistics.median(r[t]['plan_ms'] for r in rs), sum('Gather' in r[t]['shape'] for r in rs),
            sum(r['pg']['ms'] / r[t]['ms'] > 1.1 for r in rs), sum(r['pg']['ms'] / r[t]['ms'] < 1 / 1.1 for r in rs),
            '' if t not in BASE else '   plans = %s: %d' % (BASE[t], sum(r[t]['shape'] == r[BASE[t]]['shape'] for r in rs))))
    bt = collections.defaultdict(list)
    for n, r in ok.items():
        bt[n.split('-')[0]].append(r)
    print('%-4s ' % 'T' + ' '.join('%6s' % t for t in TAGS) + '   stock sum s')
    for k in sorted(bt, key=int):
        print('%-4s ' % k + ' '.join('%6.2f' % gm([r['pg']['ms'] / r[t]['ms'] for r in bt[k]]) for t in TAGS)
              + '   %.2f' % (sum(r['pg']['ms'] for r in bt[k]) / 1000))
a, b = runs[0][1], runs[1][1]
common = [n for n in a if n in b]
print('stock plans equal v2/v20 runs: %d/%d' % (sum(a[n]['pg']['shape'] == b[n]['pg']['shape'] for n in common), len(common)))
for t in TAGS:
    print('v2 = v20 plans %s: %d' % (t, sum(a[n][t].get('shape') == b[n][t].get('shape') for n in common)))
