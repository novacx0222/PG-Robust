"""Where v2's planning time goes with all-ones profiles (no variance): the
same queries planned with RobDP's features turned on one at a time.
Planning time: median of 5 EXPLAINs (parallel off).

    ONES=dir python3 overhead.py TEMPLATE ...
"""
import glob, json, os, statistics, subprocess, sys
from cmp_runs import B, PSQL

ONES = os.environ['ONES']

def base(t, n=20):
    return dict(enable_rows_dist='on', error_sample_count=str(n), error_bin_count='2', error_sample_seed='42',
                error_sample_kde_bandwidth='0', error_profile_path=os.path.join(ONES, t), robust_search='off',
                robust_select='off', robust_envelope='off', main_objective_id='3', retain_strategy_id='0',
                add_path_limit='1', retain_path_limit='0', robust_final_path_limit='1',
                robust_join_pairing='retained', robust_dominance='off', robust_penalty_tau='0.2')

STEPS = [
    ('stock', None),
    ('A rows_dist (vectors), stock search', {}),
    ('B + robust_search (dominance per scenario)', dict(robust_search='on')),
    ('C + select E[Cost], native pairing', dict(robust_search='on', robust_select='on', robust_join_pairing='native')),
    ('D + retained pairing', dict(robust_search='on', robust_select='on')),
    ('E + max Penalty (envelope, 20 rounds)', dict(robust_search='on', robust_select='on', robust_envelope='on', main_objective_id='0')),
    ('D with N=1', dict(robust_search='on', robust_select='on', error_sample_count='1')),
    ('E with N=1', dict(robust_search='on', robust_select='on', robust_envelope='on', main_objective_id='0', error_sample_count='1')),
]

def plan_ms(sql, kv):
    s = 'SET max_parallel_workers_per_gather = 0;\n'
    if kv is not None:
        s += ''.join("SET %s = '%s';\n" % x for x in kv.items())
    q = s + 'EXPLAIN (SUMMARY ON, FORMAT JSON) ' + sql.rstrip().rstrip(';') + ';\n'
    ts = []
    for _ in range(6):
        p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                           input=q, capture_output=True, text=True, timeout=900)
        ts.append(json.loads(p.stdout)[0]['Planning Time'])
    return statistics.median(ts[1:])

res = {}
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:5]:
        row = []
        for tag, kv in STEPS:
            if kv is not None:
                b = base(t); b.update(kv); kv = b
            row.append(plan_ms(sql, kv))
        res[name] = row
        print(name, ' '.join('%.2f' % x for x in row), flush=True)
print()
for t in sys.argv[1:]:
    rs = [r for n, r in res.items() if n.startswith(t + '-')]
    print('T%s' % t, ' | '.join('%s %.2f' % (STEPS[i][0].split(' ')[0], statistics.median(r[i] for r in rs)) for i in range(len(STEPS))))
