"""v20 (PG-Robust p0-release, serving port 55501 instead of v2; MODE=v2 for
v2 itself, same measurements) with the
all-ones profiles: plan vs stock, point cost, one execution (stock: after a
warm-up), planning time (median of EXPLAINs 3-8 in one session).

    ONES=dir OUT=v20ones.json python3 v20ones.py TEMPLATE ...
"""
import glob, json, os, re, statistics, subprocess, sys
from cmp_runs import B, PSQL, shape

ONES = os.environ['ONES']

def sets(t, add, ret, main, final):
    kv = dict(enable_rows_dist='on', error_sample_count='20', error_bin_count='2', error_sample_seed='42',
              error_sample_kde_bandwidth='0', error_profile_path=os.path.join(ONES, t), add_path_limit=str(add),
              retain_path_limit=str(ret), main_objective_id=str(main), retain_strategy_id='0',
              final_score_id=str(final))
    return ''.join("SET %s = '%s';\n" % x for x in kv.items())

def sets_v2(t, add, ret, main, final):
    from cmp_runs import robust
    return robust(t, main, rlimit=ret, extra=[('error_profile_path', os.path.join(ONES, t)), ('add_path_limit', str(add))])

CFGS = [('k1 o3 f3', 1, 0, 3, 3), ('k1 o3 f0', 1, 0, 3, 0), ('k1 o0 f0', 1, 0, 0, 0), ('k8 o3 f3', 8, 0, 3, 3)]
if os.environ.get('MODE') == 'v2':      # v2 has no final_score_id: the final pick is the main objective's
    CFGS = [('k1 o3 f3', 1, 0, 3, 3), ('k1 o0 f0', 1, 0, 0, 0), ('k8 o3 f3', 8, 0, 3, 3)]
    sets = sets_v2
PRE = "SET max_parallel_workers_per_gather = 0;\nSET statement_timeout = '300s';\n"

def run(q):
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=1200)
    return p.stdout, p.stderr

def one(sql, st):
    body = sql.rstrip().rstrip(';')
    out, err = run(PRE + st + ('EXPLAIN (SUMMARY ON, FORMAT JSON) %s;\n' % body) * 8 +
                   'EXPLAIN (ANALYZE, FORMAT JSON) %s;\n' % body)
    docs = [json.loads(x) for x in re.findall(r'^\[.*?^\]', out, re.S | re.M)]
    if len(docs) != 9:
        raise RuntimeError(err[-400:])
    pt = [d[0]['Planning Time'] for d in docs[:8]]
    a = docs[8][0]
    return dict(plan_ms=statistics.median(pt[2:]), ms=a['Execution Time'], cost=a['Plan']['Total Cost'],
                shape=shape(a['Plan']))

out = {}
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:10]:
        one(sql, '')
        row = {'pg': one(sql, '')}
        for tag, add, ret, main, final in CFGS:
            row[tag] = one(sql, sets(t, add, ret, main, final))
        out[name] = row
        print('%-16s pg %.1f ms plan %.2f | ' % (name, row['pg']['ms'], row['pg']['plan_ms']) + ' | '.join(
            '%s %.1f plan %.2f %s cost %.4f' % (k, row[k]['ms'], row[k]['plan_ms'], '=' if row[k]['shape'] == row['pg']['shape'] else '*',
                                                 row[k]['cost'] / row['pg']['cost']) for k, *_ in CFGS), flush=True)
        json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
