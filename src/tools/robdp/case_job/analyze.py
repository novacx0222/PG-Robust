import json, math, statistics, sys
r = json.load(open(sys.argv[1]))
N = 20
def vec(v):
    if isinstance(v, list) and len(v) == N: return [float(x) for x in v]
    x = v[0] if isinstance(v, list) else v
    return [float(x)] * N
def g(x): return '%.3g' % x
true = r['true']
tables = {}
print('## Querylets: drawn error multipliers e^x (20 scenarios)')
print('| querylet | PG sel_est | bin | e^x min | median | max | true sel / est |')
print('|---|---|---|---|---|---|---|')
rows_card = {e['rels']: e for e in r['rels']}
base = {e['rels']: e for e in r['rels'] if e['kind'] == 'base'}
def true_ratio(key):
    if '=' not in key:
        e = base[key]; return true[key] / e['rows_point'] if e['rows_point'] else float('nan')
    a, b = key.split('=')
    j = ' '.join(sorted([a, b]))
    ta, tb = true[a], true[b]
    sel_true = true[j] / (ta * tb)
    d = [d for d in r['draws'] if d['key'] == key][0]
    return sel_true / d['sel_est']
for d in r['draws']:
    m = sorted(math.exp(x) for x in d['x'])
    print('| %s | %s | %d | %s | %s | %s | %s |' % (d['key'], g(d['sel_est']), d['bin'], g(m[0]), g(statistics.median(m)), g(m[-1]), g(true_ratio(d['key']))))
print()
print('## Relations: row samples vs PostgreSQL vs truth')
print('| relation | PG estimate | samples min | median | max | truth | truth / PG | truth inside samples? |')
print('|---|---|---|---|---|---|---|---|')
for e in sorted(r['rels'], key=lambda e: (len(e['rels'].split()), e['rels'])):
    v = sorted(vec(e['rows']))
    t = true.get(e['rels'])
    inside = '-' if t is None else ('yes' if v[0] <= t <= v[-1] else ('below' if t < v[0] else 'ABOVE'))
    print('| %s | %s | %s | %s | %s | %s | %s | %s |' % (e['rels'], g(e['rows_point']), g(v[0]), g(statistics.median(v)), g(v[-1]),
          t, g(t / e['rows_point']) if t is not None and e['rows_point'] else '-', inside))
print()
def node_table(title, tree):
    print('## ' + title)
    print('| node | rels | rows point / min / med / max | startup point / min / max | total point / min / med / max | total CV | total max/min | varies (rows, startup, total) |')
    print('|---|---|---|---|---|---|---|---|')
    for d, p in tree:
        rv, sv, tv = vec(p['rows']), vec(p['startup']), vec(p['total'])
        cv = statistics.pstdev(tv) / statistics.mean(tv) if statistics.mean(tv) else 0
        print('| %s%s | %s | %s / %s / %s / %s | %s / %s / %s | %s / %s / %s / %s | %.2f | %s | %s %s %s |' % (
            '&nbsp;' * 2 * d, p['type'], p['rels'] or '-',
            g(p['rows_point']), g(min(rv)), g(statistics.median(rv)), g(max(rv)),
            g(p['startup_point']), g(min(sv)), g(max(sv)),
            g(p['total_point']), g(min(tv)), g(statistics.median(tv)), g(max(tv)), cv,
            g(max(tv) / min(tv)) if min(tv) > 0 else 'inf',
            'Y' if len(set(rv)) > 1 else '-', 'Y' if len(set(sv)) > 1 else '-', 'Y' if len(set(tv)) > 1 else '-'))
    print()
node_table('Stock PostgreSQL plan, per node', r['paths_stock'])
node_table('RobDP plan, per node', r['paths_chosen'])
ts = vec(r['paths_stock'][0][1]['total']); tr = vec(r['paths_chosen'][0][1]['total'])
print('## The two plans per scenario (total cost)')
print('| s | ' + ' | '.join(str(s) for s in range(N)) + ' |')
print('|---|' + '---|' * N)
print('| stock | ' + ' | '.join(g(x) for x in ts) + ' |')
print('| RobDP | ' + ' | '.join(g(x) for x in tr) + ' |')
print('| stock / RobDP | ' + ' | '.join(g(a / b) for a, b in zip(ts, tr)) + ' |')
print()
print('stock: point %s, mean %s, max %s; RobDP: point %s, mean %s, max %s; stock cheaper in %d of %d scenarios'
      % (g(r['paths_stock'][0][1]['total_point']), g(statistics.mean(ts)), g(max(ts)),
         g(r['paths_chosen'][0][1]['total_point']), g(statistics.mean(tr)), g(max(tr)), sum(a < b for a, b in zip(ts, tr)), N))
print('execution: stock %.1f ms, RobDP %.1f ms; planning: stock %.2f ms, RobDP %.2f ms'
      % (r['pg']['ms'], r['robdp']['ms'], r['pg']['planning_ms'], r['robdp']['planning_ms']))
