"""Sanity check: error profiles whose points all have sel_true = sel_est
(every multiplier 1, so every scenario is PostgreSQL's point estimate).
Stock (warm, one warm-up) vs RobDP for a few objectives, k = ADD+RET
(retain strategy STRAT; default 1+0), and
robust_select off; one execution each.

    ONES=dir-with-<T>/ OUT=ones.json python3 ones.py TEMPLATE ...
"""
import glob, json, os, re, sys
from cmp_runs import B, robust, explain, shape

OBJS = (0, 1, 3, 6, 8)
ONES = os.environ['ONES']

def sets(t, **kv):
    s = robust(t, 0, retain=int(os.environ.get('STRAT', '0')), rlimit=int(os.environ.get('RET', '0')),
               extra=[('error_profile_path', os.path.join(ONES, t)),
                      ('add_path_limit', os.environ.get('ADD', '1'))] + list(kv.items()))
    return s + "SET statement_timeout = '20s';\n"

def ntables(sql):
    return len(re.findall(r'\b[a-z_]+ AS [a-z_0-9]+', sql.split('FROM', 1)[1].split('WHERE')[0]))

out = {}
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:10]:
        if ntables(sql) >= 12:
            continue
        explain(sql, '')
        r = explain(sql, '')
        row = {'pg': dict(ms=r['Execution Time'], plan_ms=r['Planning Time'], cost=r['Plan']['Total Cost'],
                          shape=shape(r['Plan']))}
        cfgs = [('o%d' % o, sets(t, main_objective_id=str(o))) for o in OBJS]
        cfgs.append(('sel off', sets(t, robust_select='off')))
        for tag, st in cfgs:
            r = explain(sql, st)
            row[tag] = dict(ms=r['Execution Time'], plan_ms=r['Planning Time'], cost=r['Plan']['Total Cost'],
                            shape=shape(r['Plan']))
        out[name] = row
        print('%-16s pg %7.1f | ' % (name, row['pg']['ms']) + ' | '.join(
            '%s %7.1f%s' % (k, row[k]['ms'], '=' if row[k]['shape'] == row['pg']['shape'] else '*')
            for k, _ in cfgs), flush=True)
json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
