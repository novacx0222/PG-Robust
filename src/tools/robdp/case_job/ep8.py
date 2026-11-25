"""The paper's diversification experiment (Tables 1/2): objective E[Penalty]
with no diversification (K=0) vs K=8 retained paths chosen by E[Penalty] or
Mix, plus E[Cost] K=0 (the paper's RobDP/E[Cost]) and stock.  Real fold-2
profiles, fold-1 queries, |S| = 20, tau = 0.2.

Per configuration: plan shape, planning time, execution time (median of 3;
an identical plan already timed in this query reuses that timing) and, on
v2, the E[Penalty] score of the plan at the top join relation against the
envelope of the same planning (the paper's objective score).

    MODE=v2|v20 OUT=ep8_v2.json python3 ep8.py TEMPLATE ...
    ROBDP_PAR=2 (workers per Gather, default 0 = no parallel query), ROBDP_PAR_GUCS=a=b,c=d (more
    parallel GUCs), CONFIGS=cost0,pen0 (subset), SCORE=0 (skip the E[Penalty] scores)
"""
import glob, json, os, re, statistics, subprocess, sys, time
sys.path.insert(0, '/Users/xuanchen2026/RobDP/PG-Robust-v2/src/tools/robdp')
from robdp_dump import parse
import objectives
from cmp_runs import B, PSQL, PARSET, robust, explain, shape

MODE = os.environ['MODE']
PER = int(os.environ.get('PER', '10'))
N = 20
# name, main objective, retain strategy, retain_path_limit, add_path_limit
CONFIGS = (('cost0', 3, 0, 0, 1), ('cost8', 3, 0, 0, 8), ('pen0', 1, 0, 0, 1), ('pen8', 1, 1, 8, 1),
           ('mix8', 1, 14, 8, 1))
SCORE = MODE == 'v2' and os.environ.get('SCORE', '1') == '1'   # E[Penalty] scores (needs pen0)
if os.environ.get('CONFIGS'):   # e.g. CONFIGS=cost0,pen0
    CONFIGS = tuple(c for c in CONFIGS if c[0] in os.environ['CONFIGS'].split(','))


def ntables(sql):
    return len(re.findall(r'\b[a-z_]+ AS [a-z_0-9]+', sql.split('FROM', 1)[1].split('WHERE')[0]))


def sets(t, o, strat, ret, add):
    if MODE == 'v2':
        return robust(t, o, retain=strat, rlimit=ret, extra=[('add_path_limit', str(add))])
    prof = B + 'imdb-separate-ep-0826-2/%s-0_cardinality/error_profile' % t
    kv = dict(enable_rows_dist='on', error_sample_count='20', error_bin_count='2', error_sample_seed='42',
              error_sample_kde_bandwidth='0', error_profile_path=prof, add_path_limit=str(add),
              retain_path_limit=str(ret), main_objective_id=str(o), retain_strategy_id=str(strat),
              final_score_id=str(o))
    return ''.join("SET %s = '%s';\n" % x for x in kv.items())


def plan(sql, st):
    """EXPLAIN (SUMMARY): plan and planning time, no execution."""
    q = st + PARSET + "EXPLAIN (SUMMARY, FORMAT JSON) %s;\n" % sql.rstrip().rstrip(';')
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=900)
    if not p.stdout.strip():
        raise RuntimeError(p.stderr[-500:])
    return json.loads(p.stdout)[0]


def score(st, sql):
    """v2: E[Penalty] of the main pick at the top join relation."""
    q = (st + PARSET + "SET robust_debug_dump = 'on';\n"
         "SET client_min_messages = 'log';\nEXPLAIN (FORMAT JSON) %s;\n" % sql.rstrip().rstrip(';'))
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=900)
    ev = parse([l for l in p.stderr.splitlines() if 'ROBDP ' in l])
    keeps = [k for k in ev.get('keep', []) if k.get('pass') == 'main' and not k['partial']]
    top = max(keeps, key=lambda k: len(k['rels'].split()))
    env = {e['rels']: objectives.vec(e['cost'], N) for e in ev.get('env', [])}
    paths = {e['id']: e for e in ev.get('path', [])}
    return env, paths[top['kept'][0]], top['rels']


out = {}
t0 = time.time()
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:PER]:
        if ntables(sql) >= 12:
            continue
        row, timed, picks = {}, {}, {}
        explain(sql, '')
        for tag, o, strat, ret, add in (('pg', None, 0, 0, 1),) + CONFIGS:
            st = '' if o is None else sets(t, o, strat, ret, add)
            try:
                r = plan(sql, st)
                sh = shape(r['Plan'])
                d = dict(cost=r['Plan']['Total Cost'], shape=sh, plan_ms=r['Planning Time'])
                if sh not in timed:
                    rs = [explain(sql, st + "SET statement_timeout = '20s';\n") for _ in range(3)]
                    timed[sh] = statistics.median(x['Execution Time'] for x in rs)
                d['ms'] = timed[sh]
                if SCORE and o is not None:
                    picks[tag] = score(st, sql)
                row[tag] = d
            except Exception as e:
                row[tag] = dict(err=str(e)[-200:])
        if picks:
            env = picks['pen0'][0]   # the envelope does not depend on the objective
            for tag, (_, p, rels) in picks.items():
                row[tag]['epen'] = objectives.objective(1, p, env[rels], 0.2)
        out[name] = row
        print('%-16s %4.0fs %s' % (name, time.time() - t0, ' | '.join(
            '%s %.1f%s' % (k, v.get('ms', -1), (' s=%.0f' % v['epen']) if 'epen' in v else '')
            for k, v in row.items())), flush=True)
        json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
