"""stock vs RobDP (fold-2 profile) on fold-1 JOB instances: median of 3
executions, plan shape, planning time"""
import json, os, sys, glob, subprocess, statistics
B = '/Users/xuanchen2026/Projects/RobDP-Backup/'
PSQL = '/Users/xuanchen2026/RobDP/install/v2-release/bin/psql'
# workers per Gather; 0 (the default here) turns parallel query off
PAR = os.environ.get('ROBDP_PAR', '0')
# more parallel GUCs, ROBDP_PAR_GUCS='max_parallel_workers=16,parallel_setup_cost=100'
PARSET = "SET max_parallel_workers_per_gather = %s;\n" % PAR + ''.join(
    "SET %s = '%s';\n" % tuple(kv.split('=', 1)) for kv in os.environ.get('ROBDP_PAR_GUCS', '').split(',') if kv)

def robust(t, main=0, retain=0, rlimit=1, extra=()):
    prof = B + 'imdb-separate-ep-0826-2/%s-0_cardinality/error_profile' % t
    s = dict(enable_rows_dist='on', error_sample_count='20', error_bin_count='2', error_sample_seed='42',
             error_sample_kde_bandwidth='0', error_profile_path=prof, robust_search='on', robust_select='on',
             main_objective_id=str(main), retain_strategy_id=str(retain), add_path_limit='1',
             retain_path_limit=str(rlimit), robust_final_path_limit='1', robust_join_pairing='retained',
             robust_dominance='off', robust_penalty_tau='0.2', robust_envelope='on')
    s.update(dict(extra))
    return ''.join("SET %s = '%s';\n" % kv for kv in s.items())

def explain(sql, sets, analyze=True):
    q = sets + PARSET + "EXPLAIN (%sFORMAT JSON) %s;\n" % (
        'ANALYZE, ' if analyze else '', sql.rstrip().rstrip(';'))
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=900)
    if not p.stdout.strip():
        raise RuntimeError(p.stderr[-500:])
    return json.loads(p.stdout)[0]

def shape(n):
    kids = [shape(c) for c in n.get('Plans', [])]
    rel = n.get('Alias', '')
    par = 'Parallel ' if n.get('Parallel Aware') else ''   # never set without parallel query
    return par + n['Node Type'] + (('(' + rel + ')') if rel else '') + (('[' + ', '.join(kids) + ']') if kids else '')

def timed(sql, sets, reps=3):
    rs = [explain(sql, sets) for _ in range(reps)]
    return statistics.median(r['Execution Time'] for r in rs), rs[0]

if __name__ == '__main__':
    out = {}
    per = int(os.environ.get('PER', '10'))
    for t in sys.argv[1:]:
        qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
        for name, sql in list(qs.items())[:per]:
            row = {'sql': sql}
            for tag, sets in (('pg', ''), ('r0', robust(t, 0)), ('r3', robust(t, 3))):
                ms, r = timed(sql, sets)
                row[tag] = dict(ms=ms, plan=shape(r['Plan']), plan_ms=r['Planning Time'])
            out[name] = row
            pg = row['pg']['ms']
            print('%-15s pg %8.1f | r0 %8.1f (%5.2fx%s) | r3 %8.1f (%5.2fx%s) | plan ms pg %.1f r0 %.1f'
                  % (name, pg, row['r0']['ms'], pg / row['r0']['ms'], '' if row['r0']['plan'] != row['pg']['plan'] else ' same',
                     row['r3']['ms'], pg / row['r3']['ms'], '' if row['r3']['plan'] != row['pg']['plan'] else ' same',
                     row['pg']['plan_ms'], row['r0']['plan_ms']), flush=True)
    json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
