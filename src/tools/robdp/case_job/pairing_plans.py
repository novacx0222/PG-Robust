"""Does native pairing (PostgreSQL's join builders, as v20) give the same
plans as retained pairing? 130 fold-1 queries (as in case_job_pmax_*), max
parallel settings, one EXPLAIN per configuration: plan shape and planning
time; v20's shapes from case_job_pmax_v20.json.

    OUT=../results/pairing_plans.json python3 pairing_plans.py
"""
import glob, json, os, sys
os.environ.setdefault('MODE', 'v2')
from cmp_runs import B, PSQL, shape
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'plantime.py')).read().split('\nout = {}')[0]
g = {'__file__': os.path.abspath(__file__).replace('pairing_plans', 'plantime')}
exec(src, g)
import re, subprocess

def plan(sql, kv):
    s = ''.join("SET %s = '%s';\n" % x for x in g['MAXPAR'].items()) + ''.join("SET %s = '%s';\n" % x for x in kv.items())
    q = s + 'EXPLAIN (SUMMARY ON, FORMAT JSON) %s;\n' % sql.rstrip().rstrip(';')
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=3600)
    if 'ERROR' in p.stderr or not p.stdout.strip():
        raise RuntimeError(p.stderr[-500:])
    d = json.loads(p.stdout)[0]
    return dict(shape=shape(d['Plan']), plan_ms=d['Planning Time'], cost=d['Plan']['Total Cost'])

pm = json.load(open('../results/case_job_pmax_v2_safe.json'))
sqls = {}
for f in glob.glob(B + 'imdb-separate-ep-0826-1/*-0_cardinality/raw_data/*.json'):
    sqls.update(json.load(open(f)))
out = {}
R = g['robust']
for n in pm:
    t = n.split('-')[0]
    row = {}
    for tag, kv in (('cost0', R(t, 3, 0, 0)), ('cost0 native', R(t, 3, 0, 0, robust_join_pairing='native')),
                    ('pen8', R(t, 1, 1, 8)), ('pen8 native', R(t, 1, 1, 8, robust_join_pairing='native')),
                    ('pen8 nodedup', R(t, 1, 1, 8, robust_select_dedup='off'))):
        row[tag] = plan(sqls[n], kv)
    out[n] = row
    print(n, ' '.join('%s=%s' % (k, 'same' if row[k]['shape'] == row[k.split()[0]]['shape'] else 'DIFF') for k in row if ' ' in k), flush=True)
    json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
