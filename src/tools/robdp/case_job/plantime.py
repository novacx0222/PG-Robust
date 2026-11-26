"""Why v2 plans slower than v20: the same queries (per template, the one with
the median v2 E[Cost]x1 planning time in case_job_pmax_v2_safe.json) planned
with v2's extra features switched off one at a time, and the same on v20.
Planning time: median of EXPLAINs 2..REPS in one session.

    MODE=v2|v20 OUT=plantime_v2.json python3 plantime.py
    REPS=4 (EXPLAINs per configuration), ONLY=c1,c1n (subset), NSMALL (N of the
    'N1' configurations: 1 on v2, 4 on v20, whose minimum is 4), PRECHECK=on|off
    (v2: robust_select_precheck; unset = the server's default), VECTOR=on|off
    (v2: robust_vector_cost; unset = the server's default)
"""
import glob, json, os, re, statistics, subprocess, sys
from cmp_runs import B, PSQL

MODE = os.environ['MODE']
REPS = int(os.environ.get('REPS', '4'))
# the small N of the 'N1' configurations (v20 accepts N >= 4 only)
NSMALL = os.environ.get('NSMALL', '1' if MODE == 'v2' else '4')
MAXPAR = dict(max_parallel_workers_per_gather='8', max_parallel_workers='16', min_parallel_table_scan_size='256kB',
              min_parallel_index_scan_size='64kB', parallel_setup_cost='100', parallel_tuple_cost='0.01')
OFF = dict(max_parallel_workers_per_gather='0')


def queries():
    d = json.load(open(os.path.join(os.path.dirname(__file__), '../results/case_job_pmax_v2_safe.json')))
    by, sqls = {}, {}
    for f in glob.glob(B + 'imdb-separate-ep-0826-1/*-0_cardinality/raw_data/*.json'):
        sqls.update(json.load(open(f)))
    for n, r in d.items():
        by.setdefault(n.split('-')[0], []).append((r['cost0']['plan_ms'], n, sqls[n]))
    out = []
    for t in sorted(by, key=int):
        rs = sorted(by[t])
        out.append(rs[len(rs) // 2][1:])
    return out


def robust(t, main, strat, ret, **kw):
    prof = B + 'imdb-separate-ep-0826-2/%s-0_cardinality/error_profile' % t
    s = dict(enable_rows_dist='on', error_sample_count='20', error_bin_count='2', error_sample_seed='42',
             error_sample_kde_bandwidth='0', error_profile_path=prof, main_objective_id=str(main),
             retain_strategy_id=str(strat), add_path_limit='1', retain_path_limit=str(ret))
    if MODE == 'v2':
        s.update(robust_search='on', robust_select='on', robust_final_path_limit='1', robust_join_pairing='retained',
                 robust_dominance='off', robust_penalty_tau='0.2', robust_envelope='on')
        if os.environ.get('PRECHECK'):
            s['robust_select_precheck'] = os.environ['PRECHECK']
        if os.environ.get('VECTOR'):
            s['robust_vector_cost'] = os.environ['VECTOR']
    else:
        s.update(final_score_id=str(main))
    s.update(kw)
    return s


# name, parallel GUCs, robust GUCs (None = stock)
def configs(t):
    c = [('stock', MAXPAR, None), ('stock/off', OFF, None)]
    for par, sfx in ((MAXPAR, ''), (OFF, '/off')):
        c += [('c1' + sfx, par, robust(t, 3, 0, 0)), ('c1 N1' + sfx, par, robust(t, 3, 0, 0, error_sample_count=NSMALL)),
              ('p8' + sfx, par, robust(t, 1, 1, 8)), ('p8 N1' + sfx, par, robust(t, 1, 1, 8, error_sample_count=NSMALL))]
        if MODE == 'v2':
            c += [('c1 native' + sfx, par, robust(t, 3, 0, 0, robust_join_pairing='native')),
                  ('c1 native N1' + sfx, par, robust(t, 3, 0, 0, robust_join_pairing='native', error_sample_count=NSMALL)),
                  ('p8 native' + sfx, par, robust(t, 1, 1, 8, robust_join_pairing='native')),
                  ('p8 native N1' + sfx, par, robust(t, 1, 1, 8, robust_join_pairing='native', error_sample_count=NSMALL))]
    only = os.environ.get('ONLY')
    return [x for x in c if not only or x[0] in only.split(',')]


def plan_ms(sql, par, kv):
    s = ''.join("SET %s = '%s';\n" % x for x in par.items())
    if kv:
        s += ''.join("SET %s = '%s';\n" % x for x in kv.items())
    q = s + ('EXPLAIN (SUMMARY ON, FORMAT JSON) %s;\n' % sql.rstrip().rstrip(';')) * REPS
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=3600)
    docs = [json.loads(x) for x in re.findall(r'^\[.*?^\]', p.stdout, re.S | re.M)]
    if len(docs) != REPS:
        raise RuntimeError(p.stderr[-500:])
    if 'ERROR' in p.stderr:
        raise RuntimeError(p.stderr[-500:])
    return statistics.median(d[0]['Planning Time'] for d in docs[1:])


out = {}
for name, sql in queries():
    t = name.split('-')[0]
    row = {}
    for tag, par, kv in configs(t):
        row[tag] = plan_ms(sql, par, kv)
    out[name] = row
    print('%-16s ' % name + ' | '.join('%s %.1f' % kv for kv in row.items()), flush=True)
    json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
