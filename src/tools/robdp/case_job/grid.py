"""Objectives x k on real JOB instances (fold-1 queries, fold-2 profile),
one execution each (repeat 1).  k = add_path_limit + retain_path_limit
(retain strategy 14 Mix for 1+3).  Queries that reach geqo_threshold are
left out: GEQO bypasses RobDP's join search.

    OUT=grid.json python3 grid.py TEMPLATE ...
"""
import glob, json, os, re, sys, time
from cmp_runs import B, robust, explain

OBJS = (0, 1, 3, 6, 11, 12, 17, 8)
KS = (('1+0', 1, 0, 0), ('2+0', 2, 0, 0), ('4+0', 4, 0, 0), ('1+3 Mix', 1, 3, 14))
PER = int(os.environ.get('PER', '10'))

def ntables(sql):
    return len(re.findall(r'\b[a-z_]+ AS [a-z_0-9]+', sql.split('FROM', 1)[1].split('WHERE')[0]))

out = {}
t0 = time.time()
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:PER]:
        if ntables(sql) >= 12:
            continue
        row = {}
        r = explain(sql, "SET statement_timeout = '20s';\n")
        row['pg'] = dict(ms=r['Execution Time'], plan_ms=r['Planning Time'])
        for kn, add, ret, strat in KS:
            for o in OBJS:
                st = robust(t, o, retain=strat, rlimit=ret, extra=[('add_path_limit', str(add))])
                try:
                    r = explain(sql, st + "SET statement_timeout = '20s';\n")
                    row['%s|%d' % (kn, o)] = dict(ms=r['Execution Time'], plan_ms=r['Planning Time'],
                                                  cost=r['Plan']['Total Cost'])
                except Exception as e:
                    row['%s|%d' % (kn, o)] = dict(err=str(e)[-200:])
        out[name] = row
        print('%-16s %3.0fs pg %7.1f | %s' % (name, time.time() - t0, row['pg']['ms'], ' '.join(
            '%.0f' % row['1+0|%d' % o].get('ms', -1) for o in OBJS)), flush=True)
        json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
