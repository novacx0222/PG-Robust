"""Parallel query on vs off: ep8.py output with ROBDP_PAR=2 against the
parallel-off run (results/case_job_ep8_v2.json), for stock, E[Cost] K=0
and E[Penalty] K=0.

    python3 par_report.py PAR.json [OFF.json]
"""
import json, math, statistics, sys, collections

on = json.load(open(sys.argv[1]))
off = json.load(open(sys.argv[2] if len(sys.argv) > 2 else '../results/case_job_ep8_v2.json'))
TAGS = ('pg', 'cost0', 'pen0')
names = [n for n in on if n in off and all('ms' in on[n].get(t, {}) for t in TAGS)]


def gm(x):
    return math.exp(sum(math.log(v) for v in x) / len(x)) if x else float('nan')


print('%d queries' % len(names))
print('%-6s %10s %10s %12s %12s %10s %10s %8s' % ('cfg', 'sum off s', 'sum on s', 'stock/x off', 'stock/x on',
                                                  'plan off', 'plan on', 'Gather'))
for t in TAGS:
    print('%-6s %10.1f %10.1f %12s %12s %10.1f %10.1f %8d' % (
        t, sum(off[n][t]['ms'] for n in names) / 1000, sum(on[n][t]['ms'] for n in names) / 1000,
        '' if t == 'pg' else '%.3f' % gm([off[n]['pg']['ms'] / off[n][t]['ms'] for n in names]),
        '' if t == 'pg' else '%.3f' % gm([on[n]['pg']['ms'] / on[n][t]['ms'] for n in names]),
        statistics.median(off[n][t]['plan_ms'] for n in names),
        statistics.median(on[n][t]['plan_ms'] for n in names),
        sum('Gather' in on[n][t]['shape'] for n in names)))
print('off/on speedup from parallelism (geomean):', {t: round(gm([off[n][t]['ms'] / on[n][t]['ms'] for n in names]), 3)
                                                      for t in TAGS})
print('plan identical to stock (on):', {t: sum(on[n][t]['shape'] == on[n]['pg']['shape'] for n in names)
                                        for t in ('cost0', 'pen0')})
bt = collections.defaultdict(list)
for n in names:
    bt[n.split('-')[0]].append(n)
print('%-4s %8s %8s | %8s %8s | %8s %8s | %s' % ('T', 'cost off', 'cost on', 'pen off', 'pen on', 'pg sum off', 'on', 'Gather pg/cost/pen'))
for k in sorted(bt, key=int):
    ns = bt[k]
    print('%-4s %8.2f %8.2f | %8.2f %8.2f | %8.2f %8.2f | %d/%d/%d' % (
        k, gm([off[n]['pg']['ms'] / off[n]['cost0']['ms'] for n in ns]), gm([on[n]['pg']['ms'] / on[n]['cost0']['ms'] for n in ns]),
        gm([off[n]['pg']['ms'] / off[n]['pen0']['ms'] for n in ns]), gm([on[n]['pg']['ms'] / on[n]['pen0']['ms'] for n in ns]),
        sum(off[n]['pg']['ms'] for n in ns) / 1000, sum(on[n]['pg']['ms'] for n in ns) / 1000,
        *(sum('Gather' in on[n][t]['shape'] for n in ns) for t in TAGS)))
