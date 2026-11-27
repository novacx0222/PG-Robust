"""m1 vs m3 (vs stock) on JOB test queries with the regenerated PAR2QO
error profiles (train-50 profiles, test queries only).

Per mode (m1 = PG-Robust p0-release, m3 = PG-Robust-v2 v2-release) the
script (re)starts the server on --pgdata with that mode's binaries, then for
every selected query:
  1. one stock EXPLAIN ANALYZE to warm the cache (not recorded),
  2. stock: plan (cost, shape, planning time) and execution, --repeat times,
  3. each robust configuration: EXPLAIN (SUMMARY) for plan, point cost and
     planning time, then EXPLAIN ANALYZE --repeat times unless a plan of the
     same shape was already timed for this query in this mode.
Execution = median of the --repeat runs; a run that hits --timeout is
recorded as timeout.

Queries: the test set (<q>-0_testing.json, testing_0..199) in id order,
leaving out every query whose SQL (whitespace-normalized) occurs in the
training data (training_50, which the profiles come from, and training_400)
and repeats of an earlier test query; the first --per that remain.

Results go to --out (JSON) after every query; a rerun with the same --out
skips the (mode, query) pairs already there, so an interrupted run resumes.
--report prints the summary of an --out file, --dry_run lists the queries
and the configurations without running anything.

    python3 m1m3.py --out res.json [--templates 1,2,...] [--per 20] [--repeat 1]
        [--modes m3,m1] [--configs cost0,pen0] [--workload cardinality]
        [--port 55503] [--pgdata DIR] [--timeout 30s] [--samples 20] [--bins 2]
        [--par 8] [--base_gucs enable_bitmapscan=off]
Defaults: 8 workers per Gather, no bitmap plans in any run.
    python3 m1m3.py --out res.json --report
"""
import argparse
import json
import math
import os
import re
import statistics
import subprocess
import sys
import time

HOME = os.path.expanduser('~')
INSTALL = {'m1': f'{HOME}/RobDP/install/p0-release/bin', 'm3': f'{HOME}/RobDP/install/v2-release/bin'}
TEMPLATES = '1,2,3,4,5,11,12,13,14,21,22,23,27,28,32,33'   # the templates that use no missing table
# name: (main objective, retain strategy, retain_path_limit, add_path_limit), as case_job/ep8.py
CONFIGS = {'cost0': (3, 0, 0, 1), 'cost8': (3, 0, 0, 8), 'pen0': (1, 0, 0, 1),
           'pen8': (1, 1, 8, 1), 'mix8': (1, 14, 8, 1)}


def norm(sql):
    return ' '.join(sql.split())


def select_queries(qdir, w, t, per):
    d = f'{qdir}/{t}_{w}/raw_data'
    test = json.load(open(f'{d}/{t}_testing.json'))
    train = set()
    for f in (f'{d}/{t}_training_50.json', f'{d}/{t}_training_400.json',
              f'{HOME}/RobDP/PG-PAR2QO/code/data/imdb-new/{t}_{w}/raw_data/{t}_training_400.json'):
        if os.path.exists(f):
            train |= set(map(norm, json.load(open(f)).values()))
    out, seen = [], set()
    for name, sql in test.items():
        s = norm(sql)
        if s in train or s in seen:
            continue
        seen.add(s)
        out.append((name, sql))
    return out[:per]


def robust_sets(mode, prof, cfg, args):
    o, strat, ret, add = CONFIGS[cfg]
    kv = dict(enable_rows_dist='on', error_sample_count=str(args.samples), error_bin_count=str(args.bins),
              error_sample_seed=str(args.seed), error_sample_kde_bandwidth='0', error_profile_path=prof,
              add_path_limit=str(add), retain_path_limit=str(ret), main_objective_id=str(o),
              retain_strategy_id=str(strat))
    if mode == 'm1':
        kv['final_score_id'] = str(o)   # see grid20.py: no forced envelope pass
    else:
        kv.update(robust_search='on', robust_select='on', robust_final_path_limit='1',
                  robust_dominance='off', robust_penalty_tau='0.2', robust_envelope='on')
    for x in args.extra_gucs.split(','):
        if x:
            k, v = x.split('=', 1)
            kv[k] = v
    return ''.join("SET %s = '%s';\n" % x for x in kv.items())


class Server:
    def __init__(self, args):
        self.args = args
        self.mode = None

    def psql(self, text, timeout=None):
        b = INSTALL[self.mode or 'm3']
        return subprocess.run([f'{b}/psql', '-h', '/tmp', '-p', str(self.args.port), '-U', 'postgres', '-X', '-q',
                               '-At', '-d', self.args.db], input=text, capture_output=True, text=True,
                              timeout=timeout)

    def running_binary(self):
        pidf = f'{self.args.pgdata}/postmaster.pid'
        if not os.path.exists(pidf):
            return None
        pid = open(pidf).readline().strip()
        p = subprocess.run(['ps', '-o', 'command=', '-p', pid], capture_output=True, text=True)
        return p.stdout.split()[0] if p.stdout.strip() else None

    def use(self, mode):
        want = f'{INSTALL[mode]}/postgres'
        cur = self.running_binary()
        if cur != want:
            if cur:
                subprocess.run([os.path.join(os.path.dirname(cur), 'pg_ctl'), '-D', self.args.pgdata, '-w',
                                'stop', '-m', 'fast'], check=True, capture_output=True)
            subprocess.run([f'{INSTALL[mode]}/pg_ctl', '-D', self.args.pgdata, '-w', '-l', self.args.pglog,
                            '-o', f'-p {self.args.port} {self.args.server_opts}', 'start'],
                           check=True, capture_output=True)
        self.mode = mode
        r = self.psql('SELECT version();')
        if r.returncode != 0:
            raise RuntimeError(r.stderr)

    def explain(self, sql, sets, analyze):
        pre = "SET max_parallel_workers_per_gather = %s;\n" % self.args.par
        for x in self.args.base_gucs.split(','):   # every statement, stock included
            if x:
                pre += "SET %s = '%s';\n" % tuple(x.split('=', 1))
        if analyze:
            pre += "SET statement_timeout = '%s';\n" % self.args.timeout
        q = pre + sets + "EXPLAIN (%sSUMMARY, FORMAT JSON) %s;\n" % ('ANALYZE, ' if analyze else '',
                                                                   sql.rstrip().rstrip(';'))
        r = self.psql(q, timeout=3600)
        if not r.stdout.strip():
            if 'statement timeout' in r.stderr:
                return None
            raise RuntimeError(r.stderr[-500:])
        return json.loads(r.stdout)[0]


def shape(n):
    kids = [shape(c) for c in n.get('Plans', [])]
    rel = n.get('Alias', '')
    return n['Node Type'] + (('(' + rel + ')') if rel else '') + (('[' + ', '.join(kids) + ']') if kids else '')


def run_query(srv, sql, configs, prof, args):
    srv.explain(sql, '', True)   # warm-up, not recorded
    row, timed = {}, {}
    for tag in ['pg'] + configs:
        try:
            st = '' if tag == 'pg' else robust_sets(srv.mode, prof, tag, args)
            p = srv.explain(sql, st, False)
            sh = shape(p['Plan'])
            d = dict(cost=p['Plan']['Total Cost'], shape=sh, plan_ms=p['Planning Time'])
            if sh not in timed:
                ms = []
                for _ in range(args.repeat):
                    r = srv.explain(sql, st, True)
                    ms.append(math.inf if r is None else r['Execution Time'])
                timed[sh] = statistics.median(ms)
            d['ms'] = timed[sh]
            d['same_as_pg'] = sh == row.get('pg', d)['shape']
            row[tag] = d
        except Exception as e:
            row[tag] = dict(err=str(e)[-300:])
    return row


def gm(xs):
    xs = [x for x in xs if x and math.isfinite(x) and x > 0]
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else float('nan')


def report(res):
    rows = res['rows']
    modes = sorted({k.split('|')[0] for k in rows})
    cfgs = res['meta']['configs']
    print(f"{len(rows)} (mode, query) rows; meta: {json.dumps(res['meta'], ensure_ascii=False)}")
    for cfg in cfgs:
        print(f'\n== {cfg}: stock/robust execution (geomean, >1 = faster than stock), sums in s, planning sums in s')
        print('template  n  ' + '  '.join(f'{m}:speedup  {m}:exec  {m}:pg_exec  {m}:plan  {m}:pg_plan  {m}:same' for m in modes))
        temps = sorted({k.split('|')[1].rsplit('_testing', 1)[0] for k in rows}, key=lambda t: int(t.split('-')[0]))
        tot = {m: [[], 0, 0, 0, 0, 0] for m in modes}
        for t in temps + ['ALL']:
            line = [f'{t:8s}']
            for m in modes:
                rs = [v for k, v in rows.items() if k.startswith(m + '|') and (t == 'ALL' or k.split('|')[1].startswith(t + '_'))]
                rs = [r for r in rs if 'ms' in r.get(cfg, {}) and 'ms' in r.get('pg', {})]
                if t == 'ALL':
                    pass
                sp = gm([r['pg']['ms'] / r[cfg]['ms'] for r in rs if r[cfg]['ms'] > 0])
                line.append(f"{len(rs):3d} {sp:8.2f} {sum(r[cfg]['ms'] for r in rs) / 1e3:9.2f} {sum(r['pg']['ms'] for r in rs) / 1e3:9.2f}"
                            f" {sum(r[cfg]['plan_ms'] for r in rs) / 1e3:8.2f} {sum(r['pg']['plan_ms'] for r in rs) / 1e3:8.2f}"
                            f" {sum(r[cfg]['same_as_pg'] for r in rs):4d}")
            print('  '.join(line))
    errs = [(k, c) for k, v in rows.items() for c, d in v.items() if isinstance(d, dict) and 'err' in d]
    tos = [(k, c) for k, v in rows.items() for c, d in v.items() if isinstance(d, dict) and d.get('ms') == math.inf]
    print(f'\nerrors: {len(errs)} {errs[:5]}\ntimeouts: {len(tos)} {tos[:10]}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--templates', default=TEMPLATES)
    ap.add_argument('--per', type=int, default=20)
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--modes', default='m3,m1')
    ap.add_argument('--configs', default='cost0,cost8,pen0,pen8')
    ap.add_argument('--workload', default='cardinality')
    ap.add_argument('--profiles', default=f'{HOME}/RobDP/data/par2qo-ep/m3', help='<dir>/<workload>/<q>-0/')
    ap.add_argument('--queries', default=f'{HOME}/RobDP/data/par2qo-ep/imdb', help='<dir>/<q>-0_<workload>/raw_data/')
    ap.add_argument('--pgdata', default=f'{HOME}/RobDP/data/par2qo-ep/pgdata')
    ap.add_argument('--pglog', default=f'{HOME}/RobDP/data/par2qo-ep/pgdata.log')
    ap.add_argument('--server_opts', default='-c max_worker_processes=16 -c allow_in_place_tablespaces=on')
    ap.add_argument('--port', type=int, default=55503)
    ap.add_argument('--db', default='imdb')
    ap.add_argument('--timeout', default='30s', help='statement_timeout of every timed execution')
    ap.add_argument('--par', default='8', help='max_parallel_workers_per_gather')
    ap.add_argument('--base_gucs', default='enable_bitmapscan=off',
                    help='a=b,c=d set for every statement (stock, warm-up and robust); default: no bitmap plans')
    ap.add_argument('--samples', type=int, default=20)
    ap.add_argument('--bins', type=int, default=2)
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--extra_gucs', default='', help='a=b,c=d added to every robust configuration')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--dry_run', action='store_true')
    args = ap.parse_args()
    configs = args.configs.split(',')
    for c in configs:
        if c not in CONFIGS:
            sys.exit(f'unknown configuration {c}; known: {", ".join(CONFIGS)}')

    res = json.load(open(args.out)) if os.path.exists(args.out) else {'meta': {}, 'rows': {}}
    if args.report:
        report(res)
        return
    res['meta'] = dict(templates=args.templates, per=args.per, repeat=args.repeat, configs=configs,
                       workload=args.workload, samples=args.samples, bins=args.bins, timeout=args.timeout,
                       par=args.par, base_gucs=args.base_gucs, extra_gucs=args.extra_gucs, profiles=args.profiles)
    work = []
    for q in args.templates.split(','):
        t = f'{q}-0'
        for name, sql in select_queries(args.queries, args.workload, t, args.per):
            work.append((t, name, sql))
    if args.dry_run:
        for t in args.templates.split(','):
            names = [n for tt, n, _ in work if tt == f'{t}-0']
            print(f'{t}-0: {len(names)} queries: {", ".join(n.rsplit("_", 1)[1] for n in names)}')
        print(f'{len(work)} queries x modes {args.modes} x configs pg,{args.configs}')
        return

    srv = Server(args)
    t0 = time.time()
    for mode in args.modes.split(','):
        todo = [w for w in work if f'{mode}|{w[1]}' not in res['rows']]
        if not todo:
            continue
        srv.use(mode)
        for i, (t, name, sql) in enumerate(todo):
            q0 = time.time()
            prof = f'{args.profiles}/{args.workload}/{t}'
            row = run_query(srv, sql, configs, prof, args)
            row['wall_s'] = round(time.time() - q0, 2)
            res['rows'][f'{mode}|{name}'] = row
            tmp = args.out + '.tmp'
            json.dump(res, open(tmp, 'w'), indent=1)
            os.replace(tmp, args.out)
            print('%s %-18s %3d/%d %6.0fs | %s' % (mode, name, i + 1, len(todo), time.time() - t0, ' | '.join(
                '%s %s' % (k, ('%.1f' % v['ms']) if 'ms' in v else 'ERR') for k, v in row.items() if isinstance(v, dict))),
                flush=True)


if __name__ == '__main__':
    main()
