"""Which objectives run the envelope pass, and does running or skipping it
change anything else?  For each query and setting: the plan, planning time,
the 'env' dump events, and the main-pass dump (minus env and timing)."""
import glob, json, re, statistics, subprocess, sys
sys.path.insert(0, '/Users/xuanchen2026/RobDP/PG-Robust-v2/src/tools/robdp')
B = '/Users/xuanchen2026/Projects/RobDP-Backup/'
PSQL = '/Users/xuanchen2026/RobDP/install/v2-release/bin/psql'

def sets(t, **kv):
    s = dict(enable_rows_dist='on', error_sample_count='20', error_bin_count='2', error_sample_seed='42',
             error_sample_kde_bandwidth='0',
             error_profile_path=B + 'imdb-separate-ep-0826-2/%s-0_cardinality/error_profile' % t,
             robust_search='on', robust_select='on', main_objective_id='0', retain_strategy_id='0',
             add_path_limit='1', retain_path_limit='1', robust_final_path_limit='1',
             robust_join_pairing='retained', robust_dominance='off', robust_penalty_tau='0.2',
             robust_envelope='on', max_parallel_workers_per_gather='0')
    s.update({k: str(v) for k, v in kv.items()})
    return ''.join("SET %s = '%s';\n" % x for x in s.items())

def run(sql, st, dump=False):
    q = st + ("SET robust_debug_dump = 'on';\nSET client_min_messages = 'log';\n" if dump else '')
    q += 'EXPLAIN (FORMAT JSON, SUMMARY ON) %s;\n' % sql.rstrip().rstrip(';')
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=900)
    if not p.stdout.strip():
        raise RuntimeError(p.stderr[-800:])
    return json.loads(p.stdout)[0], p.stderr

def plan_ms(sql, st):
    run(sql, st)
    return statistics.median(run(sql, st)[0]['Planning Time'] for _ in range(3))

def main_dump(err):
    out, env = [], 0
    for l in err.splitlines():
        m = re.search(r'ROBDP (\w+)', l)
        if not m:
            continue
        if m.group(1) == 'env':
            env += 1
            continue
        l = re.sub(r'"(ms|us|time|elapsed)[a-z_]*":\s*[0-9.e+-]+,?', '', l[l.index('ROBDP'):])
        l = re.sub(r'"envelope":\s*(true|false),?', '', l)
        out.append(l)
    return out, env

QS = []
for t, k in (('2', 3), ('4', 3), ('11', 3), ('33', 2)):
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    QS += [(t, n, s) for n, s in list(qs.items())[:k]]

# 1. which objectives run the envelope rounds (retain off, so only the main objective decides)
print('== 1. envelope pass by main objective (retain_path_limit 0), planning ms')
objs = (0, 1, 2, 3, 4, 5, 6, 8, 10, 11, 12, 17, 18)
print('%-16s' % 'query' + ''.join('%8s' % ('o%d' % o) for o in objs))
for t, n, sql in QS:
    row = []
    for o in objs:
        _, err = run(sql, sets(t, main_objective_id=o, retain_path_limit=0), dump=True)
        env = main_dump(err)[1]
        row.append('%6.1f%s' % (plan_ms(sql, sets(t, main_objective_id=o, retain_path_limit=0)), '*' if env else ' '))
    print('%-16s' % n + ''.join('%8s' % c for c in row), flush=True)
print('(* = the envelope pass ran: env events in the dump)')

# 2. does the envelope pass leave anything behind?  robust_select off runs it
#    iff robust_envelope is on and uses it for nothing: the main pass must be
#    the same either way
print('\n== 2. robust_select off: robust_envelope on vs off')
for t, n, sql in QS:
    a = run(sql, sets(t, robust_select='off', robust_envelope='on'), dump=True)
    b = run(sql, sets(t, robust_select='off', robust_envelope='off'), dump=True)
    da, ea = main_dump(a[1])
    db, eb = main_dump(b[1])
    pa = plan_ms(sql, sets(t, robust_select='off', robust_envelope='on'))
    pb = plan_ms(sql, sets(t, robust_select='off', robust_envelope='off'))
    print('%-16s plan same %-5s main-pass dump same %-5s (%d lines) env events %d/%d  planning %.1f vs %.1f ms'
          % (n, a[0]['Plan'] == b[0]['Plan'], da == db, len(da), ea, eb, pa, pb), flush=True)

# 3. E[Cost] with the envelope run anyway (retain strategy 0 needs it) vs
#    skipped (retain strategy 16, no envelope), retain limit 1: the main
#    objective's scores of the candidates both runs share must agree
print('\n== 3. E[Cost] main: retain 0 (envelope runs) vs retain 16 (skipped)')
for t, n, sql in QS:
    a = run(sql, sets(t, main_objective_id=3, retain_strategy_id=0), dump=True)
    b = run(sql, sets(t, main_objective_id=3, retain_strategy_id=16), dump=True)
    print('%-16s env events %d/%d  plan same %s' % (n, main_dump(a[1])[1], main_dump(b[1])[1],
                                                   a[0]['Plan'] == b[0]['Plan']), flush=True)
