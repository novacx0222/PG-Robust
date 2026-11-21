"""robust_select_dedup on vs off with all-ones profiles: candidates the main
pass's selection sees (sum of 'cands' over its keep events), the plan, point
cost, planning time (median of 3 EXPLAINs) and one execution.

    ONES=dir OUT=dedup.json python3 dedup.py TEMPLATE ...
"""
import glob, json, os, re, statistics, subprocess, sys
from cmp_runs import B, PSQL, robust, explain, shape

ONES = os.environ['ONES']
CFGS = [('k1 o3', 1, 3), ('k1 o0', 1, 0), ('k8 o3', 8, 3)]

def sets(t, add, o, dedup):
    extra = [('error_profile_path', os.path.join(ONES, t)), ('add_path_limit', str(add)),
             ('robust_select_dedup', dedup)]
    if t == '33':
        extra.append(('geqo', 'off'))
    return robust(t, o, rlimit=0, extra=extra) + "SET max_parallel_workers_per_gather = 0;\n"

def cands(sql, st):
    q = st + "SET robust_debug_dump = on;\nSET client_min_messages = log;\nEXPLAIN " + sql.rstrip().rstrip(';') + ';\n'
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=900)
    n = 0
    for l in p.stderr.splitlines():
        if '"ev":"keep"' in l:
            e = json.loads(l[l.index('{'):])
            if e.get('pass') == 'main':
                n += len(e['cands'])
    return n

def planning(sql, st):
    q = st + 'EXPLAIN (SUMMARY ON, FORMAT JSON) ' + sql.rstrip().rstrip(';') + ';\n'
    ts = []
    for _ in range(3):
        p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                           input=q, capture_output=True, text=True, timeout=900)
        ts.append(json.loads(p.stdout)[0]['Planning Time'])
    return statistics.median(ts)

out = {}
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    cf = CFGS[:1] if t == '33' else CFGS
    for name, sql in list(qs.items())[:10]:
        row = {}
        for tag, add, o in cf:
            for dd in ('on', 'off'):
                st = sets(t, add, o, dd)
                r = explain(sql, st + "SET statement_timeout = '300s';\n")
                row['%s %s' % (tag, dd)] = dict(cands=cands(sql, st), plan_ms=planning(sql, st), ms=r['Execution Time'],
                                                cost=r['Plan']['Total Cost'], shape=shape(r['Plan']))
        out[name] = row
        print('%-16s ' % name + ' | '.join('%s: cands %d/%d plan %.1f/%.1f ms same %s' % (
            tag, row[tag + ' on']['cands'], row[tag + ' off']['cands'], row[tag + ' on']['plan_ms'], row[tag + ' off']['plan_ms'],
            row[tag + ' on']['shape'] == row[tag + ' off']['shape']) for tag, _, _ in cf), flush=True)
        json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
