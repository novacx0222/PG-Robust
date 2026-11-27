"""JOB benchmark: m0 (stock PG 16.2) vs m1 vs m3 on the PAR2QO test queries,
with one set of error profiles for both robust planners.

    python3 bench.py --out DIR [--templates 1,2,...] [--skip 7,19,24,29]
        [--per 200] [--repeat 3] [--modes m0,m1,m3] [--configs cost0,pen8]
        [--par 2] [--timeout 60s] [--base_gucs enable_bitmapscan=off,geqo=off]
        [--m1_gucs a=b,...] [--m3_gucs a=b,...] [--stock_in_modes]
    python3 bench.py --out DIR --report [--csv FILE]
    python3 bench.py --out DIR --dry_run

For every template (template-major order, so the three modes of a template
run back to back on the same cache state) and every mode, the server on
--pgdata is restarted with that mode's binaries (install/<mode>/bin; one data
directory serves all three) and for every selected query:
  1. --warmup stock EXPLAIN ANALYZE runs, not recorded;
  2. each tag (m0: pg = stock; m1/m3: the robust --configs, plus pg with
     --stock_in_modes): EXPLAIN for the plan shape, cost and planning time,
     then EXPLAIN (ANALYZE, SUMMARY, FORMAT JSON) --repeat times, unless a
     plan of the same shape was already executed for this query in this mode
     (then its times are reused, reused_from = that tag).  A run that hits
     --timeout ends the repeats of that tag; its time is the timeout
     (a lower bound, timeout = true).
  exec_ms / plan_ms = median of the runs' Execution / Planning Time,
  total_ms = plan_ms + exec_ms.
Every executed EXPLAIN ANALYZE plan is kept, complete, in
DIR/plans/<mode>/<q>-0.jsonl.gz (one JSON object per line: mode, query, tag,
run, plan; gzip members are appended, read with gzip.open).

Queries: <q>-0_testing.json (testing_0..199) in id order, leaving out the
queries whose SQL occurs in the training data (training_50, which the
profiles come from, and training_400) and repeated SQL (--keep_leaked keeps
them), the first --per.  Profiles: --profiles/<workload>/<q>-0/ (the m3
layout, which m1 reads too).

DIR/results.jsonl gets one line per (mode, query) as it finishes, so an
interrupted run resumes where it stopped (rerun the same command; a larger
--per or more templates/modes extends the run).  --report: per template and
overall, for each mode and tag against m0's stock plan: geomean speedup of
execution and of planning + execution (> 1 = faster than m0), sums of
execution, planning and total time, timeouts and plans equal to m0's.
"""
import argparse
import gzip
import json
import math
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import m1m3  # noqa: E402
from m1m3 import CONFIGS, ROOT, Server, default_bin, robust_sets, select_queries, shape  # noqa: E402

ALL_TEMPLATES = ','.join(str(t) for t in range(1, 34) if t != 29)


def queries_for(args, t):
    if not args.keep_leaked:
        return select_queries(args.queries, args.workload, t, args.per, args.released)
    test = json.load(open(f'{args.queries}/{t}_{args.workload}/raw_data/{t}_testing.json'))
    return list(test.items())[:args.per]


def tag_sets(mode, tag, prof, args):
    if tag == 'pg':
        return ''
    s = robust_sets(mode, prof, tag, args)
    for x in getattr(args, f'{mode}_gucs').split(','):
        if x:
            s += "SET %s = '%s';\n" % tuple(x.split('=', 1))
    return s


def timeout_ms(t):
    t = t.strip().lower()
    for suf, f in (('ms', 1), ('min', 60000), ('s', 1000), ('h', 3600000)):
        if t.endswith(suf):
            return float(t[:-len(suf)]) * f
    return float(t)


class PlanLog:
    def __init__(self, out, enabled):
        self.out, self.enabled, self.files = out, enabled, {}

    def write(self, mode, t, rec):
        if not self.enabled:
            return
        d = f'{self.out}/plans/{mode}'
        os.makedirs(d, exist_ok=True)
        with gzip.open(f'{d}/{t}.jsonl.gz', 'at') as f:
            f.write(json.dumps(rec) + '\n')


def run_query(srv, mode, t, name, sql, args, plans):
    prof = f'{args.profiles}/{args.workload}/{t}'
    for _ in range(args.warmup):
        srv.explain(sql, '', True)
    tags = ['pg'] if mode == 'm0' else (['pg'] if args.stock_in_modes else []) + args.configs.split(',')
    row, timed = {}, {}
    tmo = timeout_ms(args.timeout)
    for tag in tags:
        try:
            st = tag_sets(mode, tag, prof, args)
            p = srv.explain(sql, st, False)
            sh = shape(p['Plan'])
            d = dict(cost=p['Plan']['Total Cost'], shape=sh)
            plan_samples = [p['Planning Time']]
            if sh in timed:
                src = timed[sh]
                d.update(exec_ms=row[src]['exec_ms'], runs=row[src]['runs'], timeout=row[src]['timeout'],
                         reused_from=src)
            else:
                ex, to = [], False
                for i in range(args.repeat):
                    r = srv.explain(sql, st, True)
                    if r is None:
                        ex.append(tmo)
                        to = True
                        break
                    ex.append(r['Execution Time'])
                    plan_samples.append(r['Planning Time'])
                    plans.write(mode, t, dict(mode=mode, query=name, tag=tag, run=i, plan=r))
                d.update(exec_ms=statistics.median(ex), runs=ex, timeout=to)
                timed[sh] = tag
            d['plan_ms'] = statistics.median(plan_samples)
            d['total_ms'] = d['plan_ms'] + d['exec_ms']
            row[tag] = d
        except Exception as e:  # noqa: BLE001 (recorded per tag)
            row[tag] = dict(err=str(e)[-300:])
    return row


def load_rows(out):
    rows = {}
    f = f'{out}/results.jsonl'
    if os.path.exists(f):
        for line in open(f):
            if line.strip():
                r = json.loads(line)
                rows[f"{r['mode']}|{r['query']}"] = r
    return rows


def gm(xs):
    xs = [x for x in xs if x > 0 and math.isfinite(x)]
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else float('nan')


def report(args):
    rows = load_rows(args.out)
    if not rows:
        sys.exit(f'no results in {args.out}/results.jsonl')
    meta = [json.loads(x) for x in open(f'{args.out}/meta.jsonl')] if os.path.exists(f'{args.out}/meta.jsonl') else []
    if meta:
        m = meta[-1]
        print('last run: ' + ', '.join(f'{k}={m[k]}' for k in ('templates', 'per', 'repeat', 'modes', 'configs', 'par',
                                                                  'timeout', 'base_gucs', 'm1_gucs', 'm3_gucs', 'profiles')))
    base = {r['query']: r['pg'] for k, r in rows.items() if r['mode'] == 'm0' and 'exec_ms' in r.get('pg', {})}
    if not base:
        sys.exit('no m0 results: the report compares every mode with m0')
    tmpl = lambda q: q.rsplit('_testing', 1)[0]  # noqa: E731
    temps = sorted({tmpl(q) for q in base}, key=lambda t: int(t.split('-')[0]))
    series = []   # (label, mode, tag)
    for mode in ('m0', 'm1', 'm3'):
        tags = sorted({tg for r in rows.values() if r['mode'] == mode for tg in r if isinstance(r[tg], dict)},
                      key=lambda x: (x != 'pg', x))
        series += [(f'{mode}.{tg}', mode, tg) for tg in tags]
    wall = {}
    for r in rows.values():
        wall[r['mode']] = wall.get(r['mode'], 0) + r.get('wall_s', 0)

    def stats(mode, tag, t):
        qs = [q for q in base if t == 'ALL' or tmpl(q) == t]
        pairs = [(base[q], rows[f'{mode}|{q}'][tag]) for q in qs
                 if f'{mode}|{q}' in rows and 'exec_ms' in rows[f'{mode}|{q}'].get(tag, {})]
        if not pairs:
            return None
        return dict(n=len(pairs),
                    sp_exec=gm([b['exec_ms'] / x['exec_ms'] for b, x in pairs]),
                    sp_total=gm([b['total_ms'] / x['total_ms'] for b, x in pairs]),
                    exec=sum(x['exec_ms'] for _, x in pairs) / 1e3, plan=sum(x['plan_ms'] for _, x in pairs) / 1e3,
                    total=sum(x['total_ms'] for _, x in pairs) / 1e3,
                    b_total=sum(b['total_ms'] for b, _ in pairs) / 1e3,
                    to=sum(bool(x['timeout']) for _, x in pairs),
                    same=sum(x['shape'] == b['shape'] for b, x in pairs))

    print('\nOverall, against m0 stock (speedup = geomean of m0 time / this time, >1 = faster; sums in s; '
          'a timeout counts as the timeout)')
    print(f"{'series':14s} {'n':>5s} {'sp_exec':>8s} {'sp_total':>8s} {'exec_s':>9s} {'plan_s':>8s} {'total_s':>9s} "
          f"{'m0_total':>9s} {'timeouts':>8s} {'same_m0':>7s}")
    for label, mode, tag in series:
        s = stats(mode, tag, 'ALL')
        if s:
            print(f"{label:14s} {s['n']:5d} {s['sp_exec']:8.3f} {s['sp_total']:8.3f} {s['exec']:9.1f} {s['plan']:8.1f} "
                  f"{s['total']:9.1f} {s['b_total']:9.1f} {s['to']:8d} {s['same']:7d}")
    print('wall time per mode (h): ' + ', '.join(f'{m} {w / 3600:.2f}' for m, w in sorted(wall.items())))

    print('\nPer template: speedup exec / total (geomean vs m0), total s')
    print(f"{'template':8s} {'n':>4s} {'m0_tot':>8s}  " + '  '.join(f'{lb:>22s}' for lb, _, _ in series if lb != 'm0.pg'))
    for t in temps:
        b = stats('m0', 'pg', t)
        line = f"{t:8s} {b['n']:4d} {b['total']:8.1f}  "
        cells = []
        for label, mode, tag in series:
            if label == 'm0.pg':
                continue
            s = stats(mode, tag, t)
            cells.append(f"{s['sp_exec']:6.2f} {s['sp_total']:6.2f} {s['total']:8.1f}" if s else f"{'-':>22s}")
        print(line + '  '.join(cells))

    errs = [(k, tg, d['err'][:120]) for k, r in rows.items() for tg, d in r.items() if isinstance(d, dict) and 'err' in d]
    print(f'\nerrors: {len(errs)}')
    for e in errs[:10]:
        print('  ', *e)
    if args.csv:
        import csv
        with open(args.csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['template', 'query', 'mode', 'tag', 'exec_ms', 'plan_ms', 'total_ms', 'timeout', 'cost',
                        'same_as_m0', 'reused_from', 'runs'])
            for k, r in sorted(rows.items()):
                for tg, d in r.items():
                    if isinstance(d, dict) and 'exec_ms' in d:
                        b = base.get(r['query'])
                        w.writerow([tmpl(r['query']), r['query'], r['mode'], tg, round(d['exec_ms'], 3),
                                    round(d['plan_ms'], 3), round(d['total_ms'], 3), d['timeout'], d['cost'],
                                    b is not None and d['shape'] == b['shape'], d.get('reused_from', ''),
                                    ' '.join(f'{x:.3f}' for x in d['runs'])])
        print(f'per-query table: {args.csv}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True, help='results directory')
    ap.add_argument('--templates', default=ALL_TEMPLATES)
    ap.add_argument('--skip', default='', help='templates to leave out, e.g. 7,19,24,29')
    ap.add_argument('--per', type=int, default=200, help='test queries per template (after the leakage filter)')
    ap.add_argument('--keep_leaked', action='store_true', help='do not drop test queries that occur in the training data')
    ap.add_argument('--repeat', type=int, default=3)
    ap.add_argument('--warmup', type=int, default=1)
    ap.add_argument('--modes', default='m0,m1,m3')
    ap.add_argument('--configs', default='cost0,pen8', help=f'robust configurations of m1/m3: {",".join(CONFIGS)}')
    ap.add_argument('--stock_in_modes', action='store_true', help='also run the stock plan with the m1/m3 binaries')
    ap.add_argument('--workload', default='cardinality')
    ap.add_argument('--profiles', default=f'{ROOT}/data/par2qo-ep/m3', help='<dir>/<workload>/<q>-0/')
    ap.add_argument('--queries', default=f'{ROOT}/data/par2qo-ep/imdb', help='<dir>/<q>-0_<workload>/raw_data/')
    ap.add_argument('--released', default=f'{ROOT}/PG-PAR2QO/code/data/imdb-new')
    ap.add_argument('--pgdata', default=f'{ROOT}/data/par2qo-ep/pgdata')
    ap.add_argument('--pglog', default=f'{ROOT}/data/par2qo-ep/pgdata.log')
    for m in ('m0', 'm1', 'm3'):
        ap.add_argument(f'--{m}_bin', default=default_bin(m))
    ap.add_argument('--host', default='/tmp')
    ap.add_argument('--user', default='postgres')
    ap.add_argument('--port', type=int, default=55503)
    ap.add_argument('--db', default='imdb')
    ap.add_argument('--server_opts', default='-c max_worker_processes=16')
    ap.add_argument('--timeout', default='60s', help='statement_timeout of every timed execution')
    ap.add_argument('--par', default='2', help='max_parallel_workers_per_gather')
    ap.add_argument('--base_gucs', default='enable_bitmapscan=off,geqo=off', help='a=b,... for every statement')
    ap.add_argument('--samples', type=int, default=20)
    ap.add_argument('--bins', type=int, default=2)
    ap.add_argument('--seed', type=int, default=42)
    ap.add_argument('--extra_gucs', default='', help='a=b,... for the robust configurations of both m1 and m3')
    ap.add_argument('--m1_gucs', default='', help='a=b,... for the robust configurations of m1 only')
    ap.add_argument('--m3_gucs', default='', help='a=b,... for m3 only, e.g. robust_ec_forest=on')
    ap.add_argument('--no_plans', action='store_true', help='do not keep the EXPLAIN ANALYZE plans')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--csv', help='with --report: per-query table')
    ap.add_argument('--dry_run', action='store_true')
    args = ap.parse_args()
    m1m3.INSTALL.update(m0=args.m0_bin, m1=args.m1_bin, m3=args.m3_bin)
    for c in args.configs.split(','):
        if c not in CONFIGS:
            sys.exit(f'unknown configuration {c}; known: {", ".join(CONFIGS)}')
    if args.report:
        report(args)
        return
    skip = set(args.skip.split(',')) - {''}
    temps = [q for q in args.templates.split(',') if q and q not in skip]
    modes = args.modes.split(',')
    work = [(f'{q}-0', queries_for(args, f'{q}-0')) for q in temps]
    n = sum(len(qs) for _, qs in work)
    if args.dry_run:
        for t, qs in work:
            prof = f'{args.profiles}/{args.workload}/{t}'
            np_ = len([f for f in os.listdir(prof) if f.endswith('.txt')]) if os.path.isdir(prof) else 0
            print(f'{t}: {len(qs)} queries, {np_} profiles in {prof}')
        print(f'{n} queries x modes {",".join(modes)}; robust configurations {args.configs}; bins: '
              + ', '.join(f'{m}={m1m3.INSTALL[m]}' for m in modes))
        return
    for m in modes:
        if not os.path.exists(f'{m1m3.INSTALL[m]}/postgres'):
            sys.exit(f'no {m} binaries in {m1m3.INSTALL[m]} (--{m}_bin)')
    os.makedirs(args.out, exist_ok=True)
    with open(f'{args.out}/meta.jsonl', 'a') as f:
        f.write(json.dumps(dict(vars(args), templates=','.join(temps), started=time.strftime('%F %T'),
                                bins={m: m1m3.INSTALL[m] for m in modes})) + '\n')
    rows = load_rows(args.out)
    todo_total = sum(1 for t, qs in work for m in modes for name, _ in qs if f'{m}|{name}' not in rows)
    print(f'{n} queries x {len(modes)} modes; {todo_total} (mode, query) pairs to run, '
          f'{len(modes) * n - todo_total} done before; results in {args.out}', flush=True)
    srv = Server(args)
    plans = PlanLog(args.out, not args.no_plans)
    t0, done = time.time(), 0
    res = open(f'{args.out}/results.jsonl', 'a')
    for t, qs in work:
        prof = f'{args.profiles}/{args.workload}/{t}'
        for mode in modes:
            todo = [(name, sql) for name, sql in qs if f'{mode}|{name}' not in rows]
            if not todo:
                continue
            if mode != 'm0' and not os.path.isdir(prof):
                print(f'WARNING: no profiles for {t} in {prof}', flush=True)
            srv.use(mode)
            for name, sql in todo:
                q0 = time.time()
                row = run_query(srv, mode, t, name, sql, args, plans)
                row.update(mode=mode, query=name, wall_s=round(time.time() - q0, 2))
                res.write(json.dumps(row) + '\n')
                res.flush()
                rows[f'{mode}|{name}'] = row
                done += 1
                el = time.time() - t0
                eta = el / done * (todo_total - done)
                cells = ' | '.join('%s %s' % (k, ('%.0f%s' % (v['exec_ms'], '!' if v['timeout'] else ''))
                                              if 'exec_ms' in v else 'ERR')
                                   for k, v in row.items() if isinstance(v, dict))
                print('%s %-16s %5d/%d %s ETA %s | %s' % (mode, name, done, todo_total, time.strftime('%H:%M:%S', time.gmtime(el)),
                                                       '%dh%02dm' % (eta // 3600, eta % 3600 // 60), cells), flush=True)
    res.close()
    print(f'done in {(time.time() - t0) / 3600:.2f} h; report: python3 {sys.argv[0]} --out {args.out} --report')


if __name__ == '__main__':
    main()
