"""E[Cost] with add_path_limit 1 vs 8 (no retained paths), real fold-2
profiles, on v2 or on v20 (PG-Robust p0-release serving port 55501):
stock (after a warm-up), k=1, k=8, one EXPLAIN ANALYZE each: plan shape,
point cost, planning and execution time.

    MODE=v2|v20 OUT=k18_v2.json python3 k18.py TEMPLATE ...
"""
import glob, json, os, re, sys, time
from cmp_runs import B, robust, explain, shape

MODE = os.environ['MODE']
PER = int(os.environ.get('PER', '10'))

def ntables(sql):
    return len(re.findall(r'\b[a-z_]+ AS [a-z_0-9]+', sql.split('FROM', 1)[1].split('WHERE')[0]))

def sets(t, k):
    if MODE == 'v2':
        return robust(t, 3, rlimit=0, extra=[('add_path_limit', str(k))])
    prof = B + 'imdb-separate-ep-0826-2/%s-0_cardinality/error_profile' % t
    kv = dict(enable_rows_dist='on', error_sample_count='20', error_bin_count='2', error_sample_seed='42',
              error_sample_kde_bandwidth='0', error_profile_path=prof, add_path_limit=str(k),
              retain_path_limit='0', main_objective_id='3', retain_strategy_id='0', final_score_id='3')
    return ''.join("SET %s = '%s';\n" % x for x in kv.items())

out = {}
t0 = time.time()
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:PER]:
        if ntables(sql) >= 12:
            continue
        row = {}
        explain(sql, '')
        for tag, st in (('pg', ''), ('k1', sets(t, 1)), ('k8', sets(t, 8))):
            try:
                r = explain(sql, st + "SET statement_timeout = '20s';\n")
                row[tag] = dict(ms=r['Execution Time'], plan_ms=r['Planning Time'], cost=r['Plan']['Total Cost'],
                                shape=shape(r['Plan']))
            except Exception as e:
                row[tag] = dict(err=str(e)[-200:])
        out[name] = row
        print('%-16s %4.0fs %s' % (name, time.time() - t0, ' | '.join(
            '%s %.1f/%.1f' % (k, v.get('ms', -1), v.get('plan_ms', -1)) for k, v in row.items())), flush=True)
        json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
