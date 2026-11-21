"""Perfect cardinalities: a one-point profile per querylet with x = log(truth /
PostgreSQL's estimate), N = 1; RobDP by E[Cost] then plans with the true row
counts (the best plan PostgreSQL's cost model can choose).  Runs it and checks
the rel vectors equal the truth."""
import json, math, os, sys, statistics, itertools
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import case

def main():
    cj, outdir = sys.argv[1], sys.argv[2]
    r = json.load(open(cj))
    true = r['true']
    rels = {e['rels']: e for e in r['rels']}
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        os.remove(os.path.join(outdir, f))
    names = sorted({a for k in true for a in k.split()})
    keys = {}
    for a in names:
        e = rels.get(a)
        if e and e['rows_point'] and true[a] != e['rows_point']:
            keys[a] = true[a] / e['rows_point']
    for a, b in itertools.combinations(names, 2):
        j = '%s %s' % (a, b)
        if j not in true or j not in rels:
            continue
        sel_est = rels[j]['rows_point'] / (rels[a]['rows_point'] * rels[b]['rows_point'])
        sel_true = true[j] / (true[a] * true[b])
        keys['%s=%s' % (a, b)] = sel_true / sel_est
    for k, ratio in keys.items():
        with open(os.path.join(outdir, k + '.txt'), 'w') as f:
            f.write('%r %r\n' % (ratio * 1e-6, 1e-6))
    sets = ''.join("SET %s = '%s';\n" % kv for kv in [
        ('enable_rows_dist', 'on'), ('error_sample_count', '1'), ('error_bin_count', '1'),
        ('error_sample_seed', '42'), ('error_sample_kde_bandwidth', '0'), ('error_profile_path', outdir),
        ('robust_search', 'on'), ('robust_select', 'on'), ('main_objective_id', '3'), ('retain_strategy_id', '0'),
        ('add_path_limit', '1'), ('retain_path_limit', '0'), ('robust_final_path_limit', '1'),
        ('robust_join_pairing', 'retained'), ('robust_dominance', 'off'), ('robust_envelope', 'on')])
    case.explain(r['sql'], sets)
    runs = [case.explain(r['sql'], sets)[0] for _ in range(5)]
    _, err = case.explain(r['sql'], sets + "SET robust_debug_dump = 'on';\nSET client_min_messages = 'log';\n",
                          analyze=False)
    ev = case.parse([l for l in err.splitlines() if 'ROBDP ' in l])
    got = {e['rels']: case.vec(e['rows'])[0] for e in ev.get('rel', [])
           if e.get('pass') in ('base', 'main') and e.get('first', True)}
    off = {k: (got[k], true[k]) for k in true if k in got and abs(got[k] - true[k]) > 0.02 * max(true[k], 1)}
    r['truth'] = dict(ms=statistics.median(x['Execution Time'] for x in runs), plan=runs[0]['Plan'], keys=keys,
                      off=off)
    json.dump(r, open(cj, 'w'), indent=1, default=str)
    print('querylets', {k: '%.3g' % v for k, v in keys.items()})
    print('rels not at the truth (>2%%): %s' % off)
    print('perfect-card plan: %.1f ms (stock %.1f, RobDP %.1f)' % (r['truth']['ms'], r['pg']['ms'], r['robdp']['ms']))

if __name__ == '__main__':
    main()
