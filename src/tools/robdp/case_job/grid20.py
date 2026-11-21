"""v20 vs v2 on the real (fold-2) profiles, same objective x k grid as
grid.py but without Q.85 Cost (v2 only).

    MODE=v20 OUT=grid20.json python3 grid20.py TEMPLATE ...
        v20 (PG-Robust p0-release) serving port 55501: one EXPLAIN ANALYZE per
        configuration (plan shape, point cost, execution, planning time).
        final_score_id = main objective, so v20 does not force the envelope
        pass for objectives that skip it (the plan does not depend on it).
    MODE=v2shape OUT=grid2shape.json python3 grid20.py TEMPLATE ...
        v2 serving: EXPLAIN only, plan shape and point cost per configuration.
"""
import glob, json, os, re, sys, time
from cmp_runs import B, robust, explain, shape

OBJS = (0, 1, 3, 6, 11, 12, 8)
KS = (('1+0', 1, 0, 0), ('2+0', 2, 0, 0), ('4+0', 4, 0, 0), ('1+3 Mix', 1, 3, 14))
MODE = os.environ['MODE']

def ntables(sql):
    return len(re.findall(r'\b[a-z_]+ AS [a-z_0-9]+', sql.split('FROM', 1)[1].split('WHERE')[0]))

def sets_v20(t, o, add, ret, strat):
    prof = B + 'imdb-separate-ep-0826-2/%s-0_cardinality/error_profile' % t
    kv = dict(enable_rows_dist='on', error_sample_count='20', error_bin_count='2', error_sample_seed='42',
              error_sample_kde_bandwidth='0', error_profile_path=prof, add_path_limit=str(add),
              retain_path_limit=str(ret), main_objective_id=str(o), retain_strategy_id=str(strat),
              final_score_id=str(o))
    return ''.join("SET %s = '%s';\n" % x for x in kv.items())

out = {}
t0 = time.time()
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:10]:
        if ntables(sql) >= 12:
            continue
        row = {}
        if MODE == 'v20':
            explain(sql, '')
            r = explain(sql, '')
            row['pg'] = dict(ms=r['Execution Time'], plan_ms=r['Planning Time'], cost=r['Plan']['Total Cost'],
                             shape=shape(r['Plan']))
        else:
            r = explain(sql, '', analyze=False)
            row['pg'] = dict(cost=r['Plan']['Total Cost'], shape=shape(r['Plan']))
        for kn, add, ret, strat in KS:
            for o in OBJS:
                key = '%s|%d' % (kn, o)
                try:
                    if MODE == 'v20':
                        r = explain(sql, sets_v20(t, o, add, ret, strat) + "SET statement_timeout = '20s';\n")
                        row[key] = dict(ms=r['Execution Time'], plan_ms=r['Planning Time'],
                                        cost=r['Plan']['Total Cost'], shape=shape(r['Plan']))
                    else:
                        st = robust(t, o, retain=strat, rlimit=ret, extra=[('add_path_limit', str(add))])
                        r = explain(sql, st, analyze=False)
                        row[key] = dict(cost=r['Plan']['Total Cost'], shape=shape(r['Plan']))
                except Exception as e:
                    row[key] = dict(err=str(e)[-200:])
        out[name] = row
        print('%-16s %4.0fs' % (name, time.time() - t0), flush=True)
        json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
