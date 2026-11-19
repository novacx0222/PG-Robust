#!/usr/bin/env python3
"""
S4 gate: row-count vectors of base relations, inheritance children, append
relations and parameterized base scans follow the scenario model of
robustrows.h exactly.  The reference recomputes every vector in Python from
the profile files: bins (check_s2), draws (check_s3), multipliers, rows.

    check_s4.py [--install v2-release]

  R1  robust on, any profile: plans identical to stock (25/25 per profile)
  R2  trivial and all-ones profiles: rows[s] == rows_point for every rel
      and ppi event, every s
  R3  shift / two-point / lognormal profiles (bandwidth 0 and Silverman):
      every draw, rel, append and ppi vector bit-identical to the reference
  R4  instances: self-join sides, duplicate aliases ("t1#1", "t1#3") and
      UNION ALL leaves draw independently; partitions use their parent's
      querylet and are named "<parent>:<partition>"; pruned partitions are
      not summed
  R5  S3 gates (and through them S2 and S1) still pass
  R6  planning time, robust on (lognormal) vs off (informational)
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys

import common
import compare
from check_s2 import parse_file, make_bins
from check_s3 import draw
from robdp_dump import parse

RESULTS = os.path.join(common.TOOLS, 'results')
PROFILES = os.path.join(common.ROOT, 'data', 'profiles')
EXTRA = os.path.join(common.ROOT, 'data', 's4_queries')

EXTRA_QUERIES = {
    'e1_part_filter': 'SELECT count(*) FROM p WHERE v < 100',
    'e2_part_pruned': 'SELECT count(*) FROM p WHERE v < 100 AND id < 60000',
    'e3_dup_alias': 'SELECT count(*) FROM t1 WHERE a = 1 AND id IN (SELECT fk FROM t1 WHERE b < 5)',
    'e4_part_selfjoin': 'SELECT count(*) FROM p x JOIN p y ON x.id = y.v WHERE x.v < 10 AND y.v < 50',
    'e5_param_both': 'SELECT * FROM t1 JOIN t8 ON t8.fk = t1.id WHERE t1.a = 3 AND t8.b < 3',
    'e6_selfjoin_both': 'SELECT count(*) FROM t1 x JOIN t1 y ON x.fk = y.id WHERE x.a = 3 AND y.a = 3',
}


def clamp_row_est(n):
    if n > 1e100 or math.isnan(n):
        return 1e100
    if n <= 1.0:
        return 1.0
    return float(round(n))


def clamp01(p):
    return 0.0 if p < 0.0 else 1.0 if p > 1.0 else p


def make_profile_dirs():
    kinds = {'trivial': 'trivial', 'ones': 'ones', 'lognormal': 'lognormal:0.5,1.5',
             's4_shift4': 'shift:4', 's4_two': 'twopoint:0.1,10'}
    for d, kind in kinds.items():
        path = os.path.join(PROFILES, d)
        if not os.path.isdir(path):
            subprocess.run([sys.executable, os.path.join(common.TOOLS, 'profiles.py'), kind, path],
                           check=True, capture_output=True)
    if os.path.isdir(EXTRA):
        shutil.rmtree(EXTRA)
    os.makedirs(EXTRA)
    for name, sql in EXTRA_QUERIES.items():
        with open(os.path.join(EXTRA, name + '.sql'), 'w') as f:
            f.write(sql + '\n')


def dump(out, install, sets, queries=None, repeat=1):
    cmd = [sys.executable, os.path.join(common.TOOLS, 'explain_dump.py'), out, '--install', install,
           '--repeat', str(repeat)] + sum([['--set', s] for s in sets], [])
    if queries:
        cmd += ['--queries', queries]
    subprocess.run(cmd, check=True, capture_output=True)
    return json.load(open(out))


def robust_sets(prof_dir, N=20, bw=0.0, seed=42, bins=2, debug=True, search=False):
    """settings for robust planning; the S4..S6 gates check the rows and costs
    PostgreSQL's own join search gives (robust_search off), S7 on"""
    s = ['enable_rows_dist=on', 'error_sample_count=%d' % N, 'error_bin_count=%d' % bins,
         'error_sample_seed=%d' % seed, 'error_sample_kde_bandwidth=%r' % bw,
         'error_profile_path=' + prof_dir, 'robust_search=%s' % ('on' if search else 'off')]
    if debug:
        s += ['robust_debug_dump=on', 'client_min_messages=log']
    return s


def first_planning(messages):
    """the events of the first planning (up to the second "query" event)"""
    lines, seen = [], 0
    for l in messages:
        if '"ev":"query"' in l:
            seen += 1
            if seen == 2:
                break
        lines.append(l)
    return [e for e in _events(lines)]


def _events(lines):
    for l in lines:
        i = l.find('ROBDP ')
        if i >= 0:
            ev = parse([l])
            for k, v in ev.items():
                for e in v:
                    e['ev'] = k
                    yield e


class Reference:
    def __init__(self, prof_dir, N, bw, seed, nbins):
        self.dir, self.N, self.bw, self.seed, self.nbins = prof_dir, N, bw, seed, nbins
        self.cache = {}

    def profile(self, key):
        if key not in self.cache:
            pts = parse_file(os.path.join(self.dir, key + '.txt'))[0]
            self.cache[key] = (pts, make_bins(pts, self.nbins))
        return self.cache[key]

    def set_ec(self, e):
        """the pair querylets of an ecgraph event; a query level's event comes
        before its first use and after its subqueries are planned"""
        self.ec = {p['inst']: p for p in e['pairs'] if p['key'] is not None}

    def querylet(self, q):
        """(x, mult) of a dumped querylet {key, inst, sel_est, bin}, or of an
        "ec" factor {num, den}: product of num's multipliers, then divided by den's"""
        if q['key'] == 'ec':
            ms = [self.querylet(self.ec[i])[1] for i in q['num']]
            ds = [self.querylet(self.ec[i])[1] for i in q['den']]
            mult = []
            for s in range(self.N):
                f = 1.0
                for m in ms:
                    f = f * m[s]
                for m in ds:
                    f = f / m[s]
                mult.append(f)
            return None, mult
        pts, bins = self.profile(q['key'])
        _, b, _, xs, _ = draw(pts, bins, q['key'], q['inst'], q['sel_est'], self.N, self.seed, self.bw)
        assert b == q['bin'], (q, b)
        sel = q['sel_est']
        mult = []
        for x in xs:
            if sel > 0.0:
                t = clamp01(sel * math.exp(x))
                mult.append(t / sel)
            else:
                mult.append(1.0)
        return xs, mult


def vec(v, n):
    """a dumped vector (or scalar) as n floats; JSON integers load as Python ints,
    whose exact arithmetic would differ from C's doubles"""
    v = v if isinstance(v, list) and len(v) == n else [v[0] if isinstance(v, list) else v] * n
    return [float(x) for x in v]


def check_events(events, ref):
    """recompute every vector; returns (number checked, list of problems)"""
    N = ref.N
    rows_of = {}           # rels -> latest rel rows vector
    problems, n = [], 0
    ref.ec = {}
    for e in events:
        if e['ev'] == 'ecgraph':
            ref.set_ec(e)
        elif e['ev'] == 'draw':
            pts, bins = ref.profile(e['key'])
            seed, b, h, xs, _ = draw(pts, bins, e['key'], e['inst'], e['sel_est'], N, ref.seed, ref.bw)
            n += 1
            if e['seed'] != '%016x' % seed or e['bin'] != b or vec(e['x'], N) != xs:
                problems.append('draw %s/%s differs' % (e['key'], e['inst']))
        elif e['ev'] == 'rel':
            n += 1
            got = vec(e['rows'], N)
            if e['kind'] in ('base', 'other'):
                qs = e['querylets']
                if not qs:
                    want = [e['rows_point']] * N
                else:
                    _, m = ref.querylet(qs[0])
                    want = []
                    for s in range(N):
                        p = clamp01(e['sel_est'] * m[s])
                        want.append(clamp_row_est(e['tuples'] * p))
                if len(qs) > 1:
                    problems.append('rel %s: %d querylets' % (e['rels'], len(qs)))
            elif e['kind'] == 'join':
                if e['first']:
                    rows_of[e['rels']] = got
                continue
            elif e['kind'] == 'append':
                want = [0.0] * N
                for c in e['children']:
                    cv = rows_of[c]
                    for s in range(N):
                        want[s] += cv[s]
            else:
                want = got
            if got != want:
                problems.append('rel %s (%s): C %s..., Python %s...' % (e['rels'], e['kind'], got[:3], want[:3]))
            rows_of[e['rels']] = got
        elif e['ev'] == 'ppi' and e.get('kind', 'base') == 'base':
            n += 1
            got = vec(e['rows'], N)
            relrows = rows_of[e['rels']]
            ms = [ref.querylet(q)[1] for q in e['querylets']]
            if not ms:
                want = [e['rows_point']] * N
            else:
                want = []
                for s in range(N):
                    p = e['sel_est']
                    for m in ms:
                        p = p * m[s]
                    p = clamp01(p)
                    r = clamp_row_est(e['tuples'] * p)
                    want.append(min(r, relrows[s]) if r > relrows[s] else r)
            if got != want:
                problems.append('ppi %s outer %s: C %s..., Python %s...' % (e['rels'], e['outer'], got[:3], want[:3]))
    return n, problems


def same_as_stock(stock, cand):
    bad = []
    for name, qa in stock['queries'].items():
        qb = cand['queries'].get(name, {})
        if 'plan' not in qb:
            bad.append((name, 'ERROR'))
            continue
        verdict, _ = compare.compare_plans(qa['plan'], qb['plan'], 1e-9)
        if verdict != 'SAME':
            bad.append((name, verdict))
    return bad


def report(tag, ok, detail=''):
    print('%-4s %s  %s' % (tag, 'PASS' if ok else 'FAIL', detail))
    return ok


def all_events(d):
    return {name: first_planning(q.get('messages', [])) for name, q in d['queries'].items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    a = ap.parse_args()
    make_profile_dirs()
    stock = json.load(open(os.path.join(RESULTS, 'stock_v2rel.json')))
    allok = True
    P = lambda name: os.path.join(PROFILES, name)

    # R1
    bad_all, nq = [], 0
    for d in ('trivial', 'ones', 's4_shift4', 's4_two', 'lognormal'):
        res = dump(os.path.join(RESULTS, 's4_r1_%s.json' % d), a.install, robust_sets(P(d), debug=False))
        bad = same_as_stock(stock, res)
        nq += len(stock['queries'])
        bad_all += [(d,) + b for b in bad]
    allok &= report('R1', not bad_all, '%d plannings over 5 profile kinds identical to stock' % nq
                    if not bad_all else str(bad_all[:5]))

    # R2
    problems, n_ev, n_q = [], 0, 0
    for d in ('trivial', 'ones'):
        for qdir in (None, EXTRA):
            res = dump(os.path.join(RESULTS, 's4_r2.json'), a.install, robust_sets(P(d), bins=3), qdir)
            for name, evs in all_events(res).items():
                for e in evs:
                    if e['ev'] in ('rel', 'ppi'):
                        n_ev += 1
                        n_q += len(e['querylets'])
                        point = e['rows_point'] if e.get('first', True) else e['pair_point']
                        if any(v != point for v in vec(e['rows'], 20)):
                            problems.append('%s %s %s %s' % (d, name, e['ev'], e['rels']))
    allok &= report('R2', not problems and n_q > 0,
                    '%d rel/ppi events (%d querylets with a profile): rows[s] == rows_point' % (n_ev, n_q)
                    if not problems else '; '.join(problems[:4]))

    # R3
    configs = [('s4_shift4', 0.0, 42, 2, 20), ('s4_two', 0.0, 42, 3, 20), ('lognormal', 0.0, 42, 2, 20),
               ('lognormal', -1.0, 7, 4, 50), ('lognormal', 0.3, 0, 1, 1)]
    problems, counts, varying = [], 0, 0
    for d, bw, seed, nb, N in configs:
        ref = Reference(P(d), N, bw, seed, nb)
        for qdir in (None, EXTRA):
            res = dump(os.path.join(RESULTS, 's4_r3.json'), a.install, robust_sets(P(d), N, bw, seed, nb), qdir)
            for name, evs in all_events(res).items():
                try:
                    c, p = check_events(evs, ref)
                except (KeyError, AssertionError) as ex:
                    c, p = 0, ['%s: reference failed: %r' % (name, ex)]
                counts += c
                problems += ['%s %s bw=%g: %s' % (d, name, bw, x) for x in p]
                varying += sum(1 for e in evs if e['ev'] in ('rel', 'ppi') and isinstance(e['rows'], list)
                               and len(set(e['rows'])) > 1)
    allok &= report('R3', not problems and varying > 0,
                    '%d draw/rel/ppi events bit-identical over %d configurations (%d vectors vary)'
                    % (counts, len(configs), varying) if not problems else '; '.join(problems[:4]))

    # R4
    res = dump(os.path.join(RESULTS, 's4_r4.json'), a.install, robust_sets(P('lognormal'), 20), EXTRA)
    ev = all_events(res)
    res2 = dump(os.path.join(RESULTS, 's4_r4b.json'), a.install, robust_sets(P('lognormal'), 20))
    ev2 = all_events(res2)

    def rels(q, kind=None, evname='rel'):
        return {e['rels']: e for e in ev[q] if e['ev'] == evname and (kind is None or e['kind'] == kind)}

    def draws(evs):
        return {e['inst']: e for e in evs if e['ev'] == 'draw'}

    e3 = rels('e3_dup_alias')
    d3 = draws(ev['e3_dup_alias'])
    e1 = rels('e1_part_filter')
    e2 = rels('e2_part_pruned')
    e4 = rels('e4_part_selfjoin')
    d4 = draws(ev['e4_part_selfjoin'])
    d6 = draws(ev['e6_selfjoin_both'])
    d62 = draws(ev2['q62_union_all'])
    checks = {
        'duplicate alias: instances t1#1 and t1#3': {'t1#1', 't1#3'} <= set(e3) and 't1' not in e3,
        'duplicate alias: independent draws': 't1#1' in d3 and 't1#3' in d3 and d3['t1#1']['x'] != d3['t1#3']['x'],
        'self-join x, y: independent draws, join querylet x=y':
            'x' in d6 and 'y' in d6 and d6['x']['x'] != d6['y']['x'] and 'x=y' in d6,
        'partitions named p:p1..p:p4, kind other': {'p:p1', 'p:p2', 'p:p3', 'p:p4'} <= set(e1) and
            all(e1['p:p%d' % i]['kind'] == 'other' for i in range(1, 5)),
        "partitions use the parent's querylet (key p, inst p)":
            all(e1['p:p%d' % i]['querylets'][0]['inst'] == 'p' for i in range(1, 5)),
        'append p sums p1..p4': e1['p']['kind'] == 'append' and e1['p']['children'] == ['p:p%d' % i for i in range(1, 5)],
        'pruned partition p4 is not summed': e2['p']['children'] == ['p:p1', 'p:p2', 'p:p3'],
        'partitioned self-join: x:p1 and y:p1 draw as x and y':
            'x:p1' in e4 and 'y:p1' in e4 and e4['x:p1']['querylets'][0]['inst'] == 'x' and
            e4['y:p1']['querylets'][0]['inst'] == 'y' and 'x' in d4 and 'y' in d4,
        'UNION ALL leaves keep their own aliases': {'t1', 't2'} <= set(d62),
        'ppi with restriction and join querylets':
            any(e['ev'] == 'ppi' and len(e['querylets']) == 2 for e in ev['e5_param_both']),
    }
    bad = [k for k, ok in checks.items() if not ok]
    allok &= report('R4', not bad, '%d instance checks' % len(checks) if not bad else '; '.join(bad))

    # R5
    r = subprocess.run([sys.executable, os.path.join(common.TOOLS, 'check_s3.py'), '--install', a.install],
                       capture_output=True, text=True)
    allok &= report('R5', r.returncode == 0, 'check_s3: ' + ' | '.join(
        l.split()[0] + ' ' + l.split()[1] for l in r.stdout.splitlines() if l[:1] == 'D'))

    # R6
    off = dump(os.path.join(RESULTS, 's4_off.json'), a.install, ['enable_rows_dist=off'], repeat=7)
    on = dump(os.path.join(RESULTS, 's4_on.json'), a.install, robust_sets(P('lognormal'), debug=False), repeat=7)
    ratios = [on['queries'][q]['planning_ms'] / off['queries'][q]['planning_ms'] for q in off['queries']
              if off['queries'][q].get('planning_ms') and on['queries'][q].get('planning_ms')]
    gm = math.exp(sum(math.log(x) for x in ratios) / len(ratios))
    report('R6', True, 'planning time on (lognormal, N=20) / off: geometric mean %.3f (min %.2f, max %.2f)'
           % (gm, min(ratios), max(ratios)))

    common.stop()
    sys.exit(0 if allok else 1)


if __name__ == '__main__':
    main()
