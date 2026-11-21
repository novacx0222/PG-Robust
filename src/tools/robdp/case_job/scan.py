"""stock EXPLAIN ANALYZE of real JOB instances: per query, runtime and the
worst q-error over the plan's join nodes"""
import json, os, sys, glob, subprocess, re
B = '/Users/xuanchen2026/Projects/RobDP-Backup/imdb-separate-ep-0826-1'
PSQL = '/Users/xuanchen2026/RobDP/install/v2-release/bin/psql'
def run(sql, sets=''):
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=sets + "SET max_parallel_workers_per_gather = 0;\nEXPLAIN (ANALYZE, FORMAT JSON) " + sql.rstrip().rstrip(';') + ';\n',
                       capture_output=True, text=True, timeout=600)
    return json.loads(p.stdout)[0] if p.stdout.strip() else None
def nodes(n, out):
    out.append(n)
    for c in n.get('Plans', []):
        nodes(c, out)
    return out
out = {}
for t in sys.argv[1:]:
    js = glob.glob(B + '/%s-0_cardinality/raw_data/*.json' % t)
    qs = json.load(open(js[0]))
    for name, sql in list(qs.items())[:int(os.environ.get('PER', '8'))]:
        r = run(sql)
        if r is None:
            continue
        ns = nodes(r['Plan'], [])
        qe = []
        for n in ns:
            if 'Join' in n['Node Type'] or n['Node Type'] == 'Nested Loop':
                est, act = n['Plan Rows'], n['Actual Rows'] * n['Actual Loops']
                est_t = est * n['Actual Loops']
                q = max(est_t, 1) / max(act, 1)
                qe.append(q if q >= 1 else -1 / q)
        worst = max(qe, key=abs) if qe else 0
        out[name] = dict(sql=sql, ms=r['Execution Time'], worst=worst)
        print('%-16s %9.1f ms  worst join q-error %+.1f' % (name, r['Execution Time'], worst), flush=True)
json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
