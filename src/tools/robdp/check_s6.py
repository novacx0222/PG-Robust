#!/usr/bin/env python3
"""
S6 gate: the cost vectors of paths ("replay", optimizer/robustcost.h) are
PostgreSQL's own cost functions run on scenario inputs.

    check_s6.py [--install v2-release]

  C1  robust on with the dump (every path replayed), any profile: plans
      identical to stock; with parallel plans forced, identical to robust off
  C2  trivial and all-ones profiles, benchmark and extra queries, serial and
      forced parallel: every path is replayed or constant (no mismatch,
      unsupported or nocontext status) and rows[s], startup[s], total[s] equal
      the point values bit for bit
  C3  wiring, lognormal profile: a non-partial scan or join path's rows
      vector is its rel's (or ParamPathInfo's) vector; Material rows and
      Memoize rows are the child's, Memoize startup/total are the child's plus
      cpu_tuple_cost; a non-partial SeqScan's cost does not vary; children are
      costed before their parents; every value is finite and >= 0
  C4  replay against PostgreSQL: a one-point profile that scales one filter's
      selectivity by K gives, for every path the two plannings share, the
      rows, startup and total cost stock PostgreSQL computes for the query
      whose filter really selects K times as much (relative 1e-6; 1% when a
      partition's own statistics make its rows not exactly linear in the
      threshold, by at most 0.1%)
  C5  S5 gates (and through them S4..S1) still pass
  C6  planning time, robust on vs off and vs v20 E[Cost] (informational)
"""
import argparse
import json
import math
import os
import subprocess
import sys

import common
from check_s4 import (PROFILES, EXTRA, RESULTS, all_events, dump, make_profile_dirs, robust_sets,
                      same_as_stock, vec, report)
from robdp_dump import parse

N = 20
PARALLEL = ['max_parallel_workers_per_gather=4', 'parallel_setup_cost=0', 'parallel_tuple_cost=0',
            'min_parallel_table_scan_size=0', 'min_parallel_index_scan_size=0']
SCAN_JOIN = {'SeqScan', 'SampleScan', 'IndexScan', 'IndexOnlyScan', 'BitmapHeapScan', 'TidScan',
             'TidRangeScan', 'FunctionScan', 'ValuesScan', 'CteScan', 'NestLoop', 'MergeJoin', 'HashJoin'}
CPU_TUPLE_COST = 0.01


def paths(evs):
    return [e for e in evs if e['ev'] == 'path']


# ------------------------------------------------------------------ C2

def identity(evs):
    """(statuses, problems) of one planning on a trivial / ones profile"""
    st, bad = {}, []
    for e in paths(evs):
        st[e['status']] = st.get(e['status'], 0) + 1
        for f in ('rows', 'startup', 'total'):
            if any(v != e[f + '_point'] for v in vec(e[f], N)):
                bad.append('%s %s %s %s' % (e['type'], e['rels'], e['detail'], f))
                break
    return st, bad


# ------------------------------------------------------------------ C3

def wiring(evs):
    """(number of checks, problems) of one planning"""
    rel, ppi, byid = {}, {}, {}
    n, bad = 0, []

    def check(cond, msg):
        nonlocal n
        n += 1
        if not cond:
            bad.append(msg)

    for e in evs:
        if e['ev'] == 'rel' and e.get('first', True):
            rel[e['rels']] = vec(e['rows'], N)
        elif e['ev'] == 'ppi':
            ppi[(e['rels'], e['outer'])] = vec(e['rows'], N)
        elif e['ev'] == 'path':
            r, s, t = vec(e['rows'], N), vec(e['startup'], N), vec(e['total'], N)
            name = '%s %s(%s) param %s' % (e['type'], e['rels'], e['detail'], e['param'])
            check(all(math.isfinite(x) and x >= 0 for x in r + s + t), name + ': not finite / negative')
            check(all(c in byid for c in e['children']), name + ': a child costed later')
            kids = [byid[c] for c in e['children'] if c in byid]
            if e['type'] in SCAN_JOIN and not e['partial']:
                want = rel.get(e['rels']) if e['param'] is None else ppi.get((e['rels'], e['param']))
                check(want is not None and r == want, name + ': rows != rel/ppi vector')
            if e['type'] == 'Material' and kids:
                check(r == vec(kids[0]['rows'], N), name + ': rows != child rows')
            if e['type'] == 'Memoize' and kids:
                k = kids[0]
                check(r == vec(k['rows'], N) and
                      s == [x + CPU_TUPLE_COST for x in vec(k['startup'], N)] and
                      t == [x + CPU_TUPLE_COST for x in vec(k['total'], N)],
                      name + ': != child + cpu_tuple_cost')
            if e['type'] == 'SeqScan' and not e['partial']:
                check(len(set(t)) == 1 and len(set(s)) == 1, name + ': cost varies')
            byid[e['id']] = e
    return n, bad


# ------------------------------------------------------------------ C4

C4_CASES = [
    # (name, template, profiled alias, base threshold); the filter's
    # selectivity is linear in the threshold
    ('inner', 'SELECT * FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr}', 't2', 200),
    ('left', 'SELECT * FROM (SELECT * FROM t1 WHERE b < {thr}) t1 LEFT JOIN t2 ON t2.fk = t1.id', 't1', 200),
    ('full', 'SELECT count(*) FROM (SELECT * FROM t1 WHERE b < {thr}) t1 FULL JOIN t2 ON t2.fk = t1.id', 't1', 200),
    ('semi', 'SELECT count(*) FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.fk = t1.id AND t2.b < {thr})', 't2', 200),
    ('anti', 'SELECT count(*) FROM t1 WHERE NOT EXISTS (SELECT 1 FROM t2 WHERE t2.fk = t1.id AND t2.b < {thr})', 't2', 200),
    ('semi2', 'SELECT count(*) FROM t2 WHERE EXISTS (SELECT 1 FROM t1 WHERE t1.id = t2.fk AND t1.b < {thr})', 't1', 100),
    ('anti2', 'SELECT count(*) FROM t2 WHERE NOT EXISTS (SELECT 1 FROM t1 WHERE t1.id = t2.fk AND t1.b < {thr})', 't1', 100),
    ('index_filter', 'SELECT * FROM t1, t2 WHERE t1.fk = t2.id AND t2.a < {thr}', 't2', 10),
    ('chain3_mid', 'SELECT * FROM t1, t2, t3 WHERE t1.fk = t2.id AND t2.fk = t3.id AND t2.b < {thr}', 't2', 200),
    ('param_nl', 'SELECT * FROM t1 JOIN t8 ON t8.fk = t1.id WHERE t1.a < {thr}', 't1', 10),
    ('in_unique', 'SELECT count(*) FROM t1 WHERE t1.id IN (SELECT fk FROM t2 WHERE t2.b < {thr})', 't2', 200),
    ('partitioned', 'SELECT count(*) FROM p WHERE v < {thr}', 'p', 10),
    ('part_join', 'SELECT * FROM p JOIN t1 ON t1.id = p.id WHERE p.v < {thr}', 'p', 10),
]


def signatures(evs):
    """{signature: path event} of the paths whose signature is unique"""
    sig, count, out = {}, {}, {}
    for e in paths(evs):
        k = (e['type'], e['rels'], e['param'], e['npathkeys'], e['detail'], e['partial'],
             tuple(sig.get(c) for c in e['children']))
        sig[e['id']] = k
        count[k] = count.get(k, 0) + 1
        out[k] = e
    return {k: e for k, e in out.items() if count[k] == 1}


def planning(sql, pdir, install):
    q = ("SET enable_rows_dist=on; SET robust_search=off; SET error_profile_path='%s'; SET error_sample_count=1;\n"
         "SET error_sample_kde_bandwidth=0; SET robust_debug_dump=on; SET client_min_messages=log;\n"
         "EXPLAIN %s;\n" % (pdir, sql))
    _, err = common.psql(q, install, on_error_stop=False)
    evs = []
    for l in err.splitlines():
        if 'ROBDP ' in l:
            for k, v in parse([l]).items():
                for e in v:
                    e['ev'] = k
                    evs.append(e)
    return evs


def close(a, b, tol):
    return a == b or abs(a - b) <= tol * max(abs(a), abs(b))


def rows_agree(a, b, ia, ib, tol):
    """a path and all its descendants have the stock rows: the inputs of the
    cost functions agree.  They do not when the stock join selectivity itself
    moves with the filter (eqjoinsel caps a column's distinct values at the
    filtered rows), which the S5 model holds at the point."""
    return close(vec(a['rows'], 1)[0], b['rows_point'], tol) and \
        all(rows_agree(ia[c], ib[d], ia, ib, tol) for c, d in zip(a['children'], b['children']))


def c4(install):
    """returns (cases, paths compared, paths whose cost varies, paths whose rows
    differ from stock, approximate cases, problems)"""
    trivial = os.path.join(PROFILES, 'trivial')
    cases, compared, varied, other_rows, approx, problems = 0, 0, 0, 0, [], []
    for K in (0.5, 2.0, 3.0):
        pdir = os.path.join(PROFILES, 's6_scale_%g' % K)
        os.makedirs(pdir, exist_ok=True)
        for alias in ('t1', 't2', 'p'):
            with open(os.path.join(pdir, alias + '.txt'), 'w') as f:
                f.write('%r 0.2\n' % (0.2 * K))
        for name, tmpl, alias, thr in C4_CASES:
            scn = planning(tmpl.format(thr=thr), pdir, install)
            stk = planning(tmpl.format(thr=int(round(thr * K))), trivial, install)
            cases += 1
            # the filter must really scale: every base rel's rows agree.  A
            # partition's selectivity comes from its own MCV list and is
            # linear in the threshold only to within a few rows: such cases
            # are compared to 1%
            b1 = {e['rels']: vec(e['rows'], 1)[0] for e in scn if e['ev'] == 'rel' and e['kind'] in ('base', 'other')}
            b2 = {e['rels']: e['rows_point'] for e in stk if e['ev'] == 'rel' and e['kind'] in ('base', 'other')}
            if b1.keys() != b2.keys():
                problems.append('%s K=%g: different base rels' % (name, K))
                continue
            d = max(abs(b1[r] - b2[r]) / b2[r] for r in b1)
            if d > 1e-3:
                problems.append('%s K=%g: base rows differ by %.2g' % (name, K, d))
                continue
            tol = 1e-6 if d == 0 else 1e-2
            if d:
                approx.append('%s K=%g (rows %.1e)' % (name, K, d))
            s1, s2 = signatures(scn), signatures(stk)
            i1 = {e['id']: e for e in paths(scn)}
            i2 = {e['id']: e for e in paths(stk)}
            n_here = 0
            for k in s1.keys() & s2.keys():
                a, b = s1[k], s2[k]
                if a['status'] not in ('replay', 'const') or a.get('semi_frozen'):
                    continue
                if not rows_agree(a, b, i1, i2, tol):
                    other_rows += 1
                    continue
                got = [vec(a[f], 1)[0] for f in ('rows', 'startup', 'total')]
                want = [b[f + '_point'] for f in ('rows', 'startup', 'total')]
                compared += 1
                n_here += 1
                if got[2] != a['total_point'] or got[1] != a['startup_point']:
                    varied += 1
                if not all(close(x, y, tol) for x, y in zip(got, want)):
                    problems.append('%s K=%g %s %s(%s): replay %s, stock %s'
                                    % (name, K, a['type'], a['rels'], a['detail'], got, want))
            if n_here == 0:
                problems.append('%s K=%g: no common path' % (name, K))
    return cases, compared, varied, other_rows, approx, problems


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    a = ap.parse_args()
    make_profile_dirs()
    stock = json.load(open(os.path.join(RESULTS, 'stock_v2rel.json')))
    P = lambda name: os.path.join(PROFILES, name)
    allok = True

    # C1
    bad_all, nq = [], 0
    for d in ('trivial', 's4_two', 'lognormal'):
        res = dump(os.path.join(RESULTS, 's6_c1.json'), a.install, robust_sets(P(d)))
        bad_all += [(d,) + b for b in same_as_stock(stock, res)]
        nq += len(stock['queries'])
    off = dump(os.path.join(RESULTS, 's6_c1_paroff.json'), a.install, PARALLEL + ['enable_rows_dist=off'])
    on = dump(os.path.join(RESULTS, 's6_c1_paron.json'), a.install, PARALLEL + robust_sets(P('lognormal')))
    bad_all += [('parallel',) + b for b in same_as_stock(off, on)]
    npar = sum(1 for q in off['queries'].values() if 'Gather' in json.dumps(q.get('plan', {})))
    allok &= report('C1', not bad_all, '%d plannings (dump on) identical to stock; %d forced-parallel plannings '
                    '(%d with Gather) identical to robust off' % (nq, len(off['queries']), npar)
                    if not bad_all else str(bad_all[:5]))

    # C2
    status, problems, nrep = {}, [], 0
    for d in ('trivial', 'ones'):
        for qdir, extra in ((None, []), (EXTRA, []), (None, PARALLEL)):
            res = dump(os.path.join(RESULTS, 's6_c2.json'), a.install, extra + robust_sets(P(d), bins=3), qdir)
            for name, evs in all_events(res).items():
                st, bad = identity(evs)
                for k, v in st.items():
                    status[k] = status.get(k, 0) + v
                problems += ['%s %s: %s' % (d, name, b) for b in bad]
    nrep = status.get('replay', 0)
    odd = {k: v for k, v in status.items() if k not in ('replay', 'const')}
    allok &= report('C2', not problems and not odd and nrep > 0,
                    '%d paths replayed, %d constant: rows/startup/total[s] == point, no other status'
                    % (nrep, status.get('const', 0)) if not problems and not odd
                    else 'status %s; %s' % (odd, '; '.join(problems[:4])))

    # C3
    n_chk, problems, n_vary = 0, [], 0
    for qdir, extra in ((None, []), (EXTRA, []), (None, PARALLEL)):
        res = dump(os.path.join(RESULTS, 's6_c3.json'), a.install, extra + robust_sets(P('lognormal')), qdir)
        for name, evs in all_events(res).items():
            c, bad = wiring(evs)
            n_chk += c
            problems += ['%s: %s' % (name, b) for b in bad]
            n_vary += sum(1 for e in paths(evs) if len(set(vec(e['total'], N))) > 1)
    allok &= report('C3', not problems and n_vary > 0,
                    '%d wiring checks hold (%d paths with a varying total cost)' % (n_chk, n_vary)
                    if not problems else '%d failures: %s' % (len(problems), '; '.join(problems[:4])))

    # C4
    common.start(a.install)
    cases, compared, varied, other_rows, approx, problems = c4(a.install)
    allok &= report('C4', not problems and compared > 0 and varied > 0 and len(approx) * 4 < cases
                    and other_rows * 10 < compared,
                    '%d cases, %d common paths equal to stock PostgreSQL to 1e-6 (%d with a cost different '
                    'from the point; %d partitioned cases to 1%%); %d paths not compared: stock rows differ '
                    '(join selectivity moves with the filter)' % (cases, compared, varied, len(approx), other_rows)
                    if not problems else '%d failures: %s' % (len(problems), '; '.join(problems[:4])))
    if approx:
        print('       to 1%: ' + ', '.join(approx))

    # C5
    common.stop()
    r = subprocess.run([sys.executable, os.path.join(common.TOOLS, 'check_s5.py'), '--install', a.install],
                       capture_output=True, text=True)
    allok &= report('C5', r.returncode == 0, 'check_s5: ' + ' | '.join(
        l.split()[0] + ' ' + l.split()[1] for l in r.stdout.splitlines() if l[:1] == 'J'))

    # C6
    common.stop()
    off = dump(os.path.join(RESULTS, 's6_off.json'), a.install, ['enable_rows_dist=off'], repeat=9)
    on = dump(os.path.join(RESULTS, 's6_on.json'), a.install, robust_sets(P('trivial'), debug=False), repeat=9)
    v20 = os.path.join(RESULTS, 'p0_trivial_obj3.json')
    v20 = json.load(open(v20)) if os.path.exists(v20) else None
    r_off, r_v20 = {}, {}
    for qn in off['queries']:
        t_on = on['queries'][qn].get('planning_ms')
        if t_on and off['queries'][qn].get('planning_ms'):
            r_off[qn] = t_on / off['queries'][qn]['planning_ms']
        if t_on and v20 and v20['queries'].get(qn, {}).get('planning_ms'):
            r_v20[qn] = t_on / v20['queries'][qn]['planning_ms']
    gm = lambda r: math.exp(sum(math.log(x) for x in r.values()) / len(r))
    w1 = max(r_off, key=r_off.get)
    msg = 'planning time on (trivial, N=20) / off: geometric mean %.2f, max %.2f (%s)' % (gm(r_off), r_off[w1], w1)
    if r_v20:
        w2 = max(r_v20, key=r_v20.get)
        msg += '; / v20 E[Cost]: geometric mean %.2f, max %.2f (%s)' % (gm(r_v20), r_v20[w2], w2)
    report('C6', True, msg)
    common.stop()
    sys.exit(0 if allok else 1)


if __name__ == '__main__':
    main()
