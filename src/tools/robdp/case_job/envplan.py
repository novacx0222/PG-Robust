"""Is the plan RobDP picks a lower-envelope plan?  For the top join
relation: the path the main objective keeps vs C*(s), the cheapest total
cost of the stock DP in scenario s (the envelope rounds, taken from an
E[Penalty] planning; the envelope does not depend on the objective).  A
lower-envelope plan is (fuzzily) the cheapest in some scenario.

    python3 envplan.py TEMPLATE ...
"""
import glob, json, re, sys, subprocess, collections
sys.path.insert(0, '/Users/xuanchen2026/RobDP/PG-Robust-v2/src/tools/robdp')
from robdp_dump import parse
import objectives
from cmp_runs import B, robust, PSQL

N = 20

def ntables(sql):
    return len(re.findall(r'\b[a-z_]+ AS [a-z_0-9]+', sql.split('FROM', 1)[1].split('WHERE')[0]))

def dump(t, o, sql):
    q = (robust(t, o) + "SET max_parallel_workers_per_gather = 0;\nSET robust_debug_dump = 'on';\n"
         "SET client_min_messages = 'log';\nEXPLAIN (FORMAT JSON) %s;\n" % sql.rstrip().rstrip(';'))
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=900)
    return parse([l for l in p.stderr.splitlines() if 'ROBDP ' in l])

def top_kept(ev):
    keeps = [k for k in ev.get('keep', []) if k.get('pass') == 'main' and not k['partial']]
    top = max(keeps, key=lambda k: len(k['rels'].split()))
    paths = {e['id']: e for e in ev.get('path', [])}
    return top['rels'], objectives.vec(paths[top['kept'][0]]['total'], N)

res = collections.Counter()
rows = []
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:10]:
        if ntables(sql) >= 12:
            continue
        evp = dump(t, 1, sql)
        env = {e['rels']: objectives.vec(e['cost'], N) for e in evp.get('env', [])}
        line = [name]
        for o, nm in ((3, 'E[Cost]'), (1, 'E[Pen]'), (0, 'maxPen')):
            ev = evp if o == 1 else dump(t, o, sql)
            rels, tot = top_kept(ev)
            c = env[rels]
            r = [tot[s] / c[s] for s in range(N)]
            m = min(r)
            res[nm + ' plans'] += 1
            res[nm + ' lower-envelope (<=1.01x somewhere)'] += m <= 1.01
            res[nm + ' exact envelope (<=1.000001x)'] += m <= 1.000001
            line.append('%s min %.3f max %.2f' % (nm, m, max(r)))
        print('  '.join(line), flush=True)
for k in sorted(res):
    print(k, res[k])
