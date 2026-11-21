"""Settings sensitivity of RobDP's plan on one case: objective, KDE
bandwidth, bins, N, seed.  Median of 3 executions after a warm-up."""
import json, os, re, statistics, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import case

def sig(n):
    """compact plan shape: join operators with their inputs"""
    t = n['Node Type']
    kids = [sig(c) for c in n.get('Plans', [])]
    if t in ('Seq Scan', 'Index Scan', 'Index Only Scan', 'Bitmap Heap Scan'):
        return {'Seq Scan': 'S', 'Index Scan': 'I', 'Index Only Scan': 'I', 'Bitmap Heap Scan': 'B'}[t] + ':' + n['Alias']
    if t in ('Hash', 'Memoize', 'Materialize', 'Aggregate', 'Sort', 'Bitmap Index Scan'):
        return kids[0] if kids else ''
    op = {'Nested Loop': 'NL', 'Hash Join': 'HJ', 'Merge Join': 'MJ'}.get(t, t)
    return '%s(%s)' % (op, ', '.join(k for k in kids if k))

def run(sql, sets):
    case.explain(sql, sets)
    rs = [case.explain(sql, sets)[0] for _ in range(3)]
    return statistics.median(x['Execution Time'] for x in rs), statistics.median(x['Planning Time'] for x in rs), rs[0]['Plan']

CFGS = [('obj 0 max Penalty (default)', {}),
        ('obj 1 E[Penalty]', {'main_objective_id': '1'}),
        ('obj 3 E[Cost]', {'main_objective_id': '3'}),
        ('obj 12 Quant0.85[Penalty]', {'main_objective_id': '12'}),
        ('obj 8 point cost', {'main_objective_id': '8'}),
        ('bandwidth Silverman', {'error_sample_kde_bandwidth': '-1'}),
        ('bins 1', {'error_bin_count': '1'}),
        ('bins 4', {'error_bin_count': '4'}),
        ('N 50', {'error_sample_count': '50'}),
        ('N 100', {'error_sample_count': '100'}),
        ('seed 1', {'error_sample_seed': '1'}),
        ('seed 2', {'error_sample_seed': '2'}),
        ('seed 3', {'error_sample_seed': '3'})]

def main():
    out = {}
    for cj in sys.argv[1:]:
        r = json.load(open(cj))
        base = case.robust_sets(r['template'])
        rows = []
        print('== %s  stock %.1f ms %s  perfect-card %.1f ms %s' % (r['name'], r['pg']['ms'], sig(r['pg']['plan']),
                                                                   r['truth']['ms'], sig(r['truth']['plan'])))
        for name, kv in CFGS:
            s = base
            for k, v in kv.items():
                s = re.sub(r"SET %s = '[^']*';" % k, "SET %s = '%s';" % (k, v), s)
            ms, pms, plan = run(r['sql'], s)
            rows.append(dict(cfg=name, ms=ms, planning_ms=pms, sig=sig(plan), cost=plan['Total Cost']))
            print('%-28s %8.1f ms  plan %6.1f ms  cost %9.0f  %s' % (name, ms, pms, plan['Total Cost'], sig(plan)))
        out[r['name']] = rows
    json.dump(out, open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sens.json'), 'w'), indent=1)

if __name__ == '__main__':
    main()
