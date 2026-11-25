"""Candidates collected by the main pass with its prechecks off (as v2 runs)
vs on: per query and objective, the candidates of the main pass's keep
events, the plan, its point cost, planning time, one execution.  The
server must run with or without ROBDP_TMP_PRECHECK_ON (a measurement-only
build switch).

    MODE=off|on OUT=precheck_off.json python3 precheck.py TEMPLATE ...
"""
import glob, json, os, sys, subprocess, re
sys.path.insert(0, '/Users/xuanchen2026/RobDP/PG-Robust-v2/src/tools/robdp')
from robdp_dump import parse
from cmp_runs import B, robust, PSQL, shape, explain

PER = int(os.environ.get('PER', '5'))
out = {}
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:PER]:
        row = {}
        for o in (3, 1):
            st = robust(t, o)
            q = (st + "SET max_parallel_workers_per_gather = 0;\nSET robust_debug_dump = 'on';\n"
                 "SET client_min_messages = 'log';\nEXPLAIN (FORMAT JSON) %s;\n" % sql.rstrip().rstrip(';'))
            p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                               input=q, capture_output=True, text=True, timeout=900)
            ev = parse([l for l in p.stderr.splitlines() if 'ROBDP ' in l])
            keeps = [k for k in ev.get('keep', []) if k.get('pass') == 'main']
            ncand = sum(len(k['cands']) for k in keeps)
            explain(sql, st, analyze=False)
            r = explain(sql, st + "SET statement_timeout = '20s';\n")
            row[str(o)] = dict(cands=ncand, keeps=len(keeps), shape=shape(r['Plan']), cost=r['Plan']['Total Cost'],
                               plan_ms=r['Planning Time'], ms=r['Execution Time'])
        out[name] = row
        print('%-16s %s' % (name, ' | '.join('o%s cands %d plan %.1f ms' % (o, v['cands'], v['plan_ms'])
                                             for o, v in row.items())), flush=True)
        json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
