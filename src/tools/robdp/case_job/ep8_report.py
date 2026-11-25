"""Summarize ep8.py output: per configuration the exec geomean vs stock,
exec sum, planning median, mean E[Penalty] score (v2), and per template.

    python3 ep8_report.py ep8_v2.json [ep8_v20.json]
"""
import json, math, statistics, sys, collections

TAGS = ('cost0', 'pen0', 'pen8', 'mix8')


def gm(x):
    return math.exp(sum(math.log(v) for v in x) / len(x)) if x else float('nan')


for f in sys.argv[1:]:
    d = json.load(open(f))
    ok = {n: r for n, r in d.items() if all('ms' in r.get(t, {}) for t in ('pg',) + TAGS)}
    print('== %s: %d queries (%d with errors skipped)' % (f, len(ok), len(d) - len(ok)))
    print('%-6s %8s %9s %8s %10s %8s %8s' % ('cfg', 'gm', 'gm-noT23', 'sum s', 'plan med', 'E[Pen]', 'same/pen0'))
    print('%-6s %8s %9s %8.1f %10.1f' % ('pg', '', '', sum(r['pg']['ms'] for r in ok.values()) / 1000,
                                         statistics.median(r['pg']['plan_ms'] for r in ok.values())))
    for t in TAGS:
        rat = [r['pg']['ms'] / r[t]['ms'] for r in ok.values()]
        rat2 = [r['pg']['ms'] / r[t]['ms'] for n, r in ok.items() if not n.startswith('23-')]
        ep = [r[t]['epen'] for r in ok.values() if 'epen' in r[t]]
        same = sum(r[t]['shape'] == r['pen0']['shape'] for r in ok.values())
        print('%-6s %8.3f %9.3f %8.1f %10.1f %8s %8d' % (
            t, gm(rat), gm(rat2), sum(r[t]['ms'] for r in ok.values()) / 1000,
            statistics.median(r[t]['plan_ms'] for r in ok.values()),
            '%.0f' % statistics.mean(ep) if ep else '-', same))
    # paper Table 1 style: per-template mean score, % improvement over pen0, averaged over templates
    bt = collections.defaultdict(list)
    for n, r in ok.items():
        bt[n.split('-')[0]].append(r)
    if any('epen' in r['pen0'] for r in ok.values()):
        for t in ('cost0', 'pen8', 'mix8'):
            imp = []
            for rs in bt.values():
                a = statistics.mean(r['pen0']['epen'] for r in rs)
                b = statistics.mean(r[t]['epen'] for r in rs)
                if a > 0:
                    imp.append((a - b) / a)
            print('E[Pen] score improvement over pen0, %s: %.1f%% (avg over %d templates with pen0 score > 0)'
                  % (t, 100 * statistics.mean(imp), len(imp)))
        better = {t: sum(r[t]['epen'] < r['pen0']['epen'] - 1e-6 for r in ok.values()) for t in ('pen8', 'mix8')}
        worse = {t: sum(r[t]['epen'] > r['pen0']['epen'] + 1e-6 for r in ok.values()) for t in ('pen8', 'mix8')}
        print('queries with lower / higher E[Pen] score than pen0:', {t: (better[t], worse[t]) for t in better})
    print('%-5s ' % 'T' + ' '.join('%7s' % t for t in TAGS) + '   (exec geomean stock/RobDP)')
    for k in sorted(bt, key=int):
        print('%-5s ' % k + ' '.join('%7.2f' % gm([r['pg']['ms'] / r[t]['ms'] for r in bt[k]]) for t in TAGS))
