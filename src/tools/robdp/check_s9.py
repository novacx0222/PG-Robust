#!/usr/bin/env python3
"""
S9 gate: robust planning above the join search (optimizer/robustselect.h):
the cost vectors of the upper paths (Agg, Group, Unique, WindowAgg, SetOp,
Limit, LockRows, ModifyTable), the scoring of the scan/join relation once
its final target is applied (top Gather paths, the only relation of a query
without joins), and the final pick of each query level by the main
objective (get_cheapest_fractional_path).

    check_s9.py [--install v2-release] [--light] [--skip-s8] [--time-only]

  U1  identity: trivial and all-ones profiles, check_s8 G1's identity
      settings, benchmark, extra, forced parallel, S8 and S9 queries: plans
      identical to stock; every upper path replayed or constant (but the
      types replay does not know: GroupingSets, RecursiveUnion), its vectors
      equal to its point values bit for bit; no final pick has vectors, so
      PostgreSQL's choice stands
  U2  upper replay against PostgreSQL (as check_s6 C4): a one-point profile
      that scales one filter's selectivity by K gives, for every upper path
      the two plannings share, the rows, startup and total cost stock
      PostgreSQL computes for the query whose filter selects K times as much
  U3  lognormal, several settings: no upper path fails its replay (mismatch,
      nocontext); where the number of groups comes from
  U4  every final pick recomputed: F(s) = the cost of fetching the fraction
      of the rows in s, envelope = the cheapest F(s) of the candidates, the
      main objective's scores, the pick; the candidates are the final
      relation's paths, their costs raised by the initPlans' cost
      (charge events: u22 .. u24) and by the targets put into them in
      place (project events: u25, u26)
  U5  the scan/join relation's scoring (keep events of the upper pass)
      recomputed against its own envelope; it covers the top Gather paths
      and the only relation of queries without joins
  U6  the final relation keeping K = 4 paths, and LIMIT: how often the final
      pick differs from PostgreSQL's choice, and the plans K changes
      (informational)
  U7  check_s8 (and through it S7 .. S1) still passes
  U8  planning time / stock (informational)
"""
import argparse
import math
import os
import subprocess
import sys

import common
import compare
import objectives
from check_s4 import PROFILES, EXTRA, RESULTS, all_events, dump, make_profile_dirs, same_as_stock, vec, \
    report
from check_s6 import PARALLEL, close, rows_agree, signatures
from check_s7 import gm
from check_s8 import BIG, S8Q, PWJ, LIGHT, N, make_queries, mean, objective, s8_sets, vmax
from robdp_dump import parse

S9Q = os.path.join(common.ROOT, 'data', 's9_queries')
S9_QUERIES = {
    'u01_group': 'SELECT t1.a, count(*) FROM t1 JOIN t2 ON t2.fk = t1.id GROUP BY t1.a',
    'u02_having': 'SELECT t1.a, t2.b, sum(t2.id) FROM t1 JOIN t2 ON t2.fk = t1.id WHERE t1.a < 50 '
                  'GROUP BY t1.a, t2.b HAVING count(*) > 1',
    'u03_distinct': 'SELECT DISTINCT t2.b FROM t1 JOIN t2 ON t2.fk = t1.id WHERE t1.a < 10',
    'u04_order_limit': 'SELECT t1.id, t2.b FROM t1 JOIN t2 ON t2.fk = t1.id WHERE t1.a < 10 ORDER BY t1.id LIMIT 50',
    'u05_single': 'SELECT * FROM t1 WHERE a < 3 AND b < 5',
    'u06_single_group': 'SELECT a, count(*) FROM t1 WHERE b < 50 GROUP BY a ORDER BY a',
    'u07_window': 'SELECT t1.a, row_number() OVER (PARTITION BY t1.a ORDER BY t2.id) FROM t1 JOIN t2 '
                  'ON t2.fk = t1.id WHERE t1.a < 5',
    'u08_union': 'SELECT a FROM t1 WHERE b < 3 UNION SELECT fk FROM t2 WHERE b < 3',
    'u09_intersect': 'SELECT a FROM t1 WHERE b < 30 INTERSECT SELECT fk FROM t2 WHERE b < 30',
    'u10_count': 'SELECT count(*) FROM t1 JOIN t2 ON t2.fk = t1.id',
    'u11_gsets': 'SELECT t1.a, t2.b, count(*) FROM t1 JOIN t2 ON t2.fk = t1.id GROUP BY GROUPING SETS ((t1.a), (t2.b))',
    'u12_subplan': 'SELECT t1.id, (SELECT max(t2.b) FROM t2 WHERE t2.fk = t1.id) FROM t1 WHERE t1.a < 3',
    'u13_exists_or': 'SELECT * FROM t1 WHERE t1.a < 5 AND (EXISTS (SELECT 1 FROM t2 WHERE t2.fk = t1.id '
                     'AND t2.b < 10) OR t1.b < 2)',
    'u14_limit_join3': 'SELECT t1.id, t3.b FROM t1 JOIN t2 ON t2.fk = t1.id JOIN t3 ON t3.id = t2.fk '
                       'WHERE t1.a < 20 ORDER BY t2.id LIMIT 10',
    'u15_minmax': 'SELECT max(id) FROM t1 WHERE a < 50',
    'u16_sub_group_limit': 'SELECT * FROM (SELECT t1.a, count(*) AS c FROM t1 JOIN t2 ON t2.fk = t1.id '
                           'GROUP BY t1.a) s WHERE c > 5 ORDER BY a LIMIT 5',
    'u17_for_update': 'SELECT * FROM t1 WHERE a < 2 AND b < 100 FOR UPDATE',
    'u18_update': 'UPDATE t1 SET c = c + 1 WHERE a < 2 AND b < 100',
    'u19_limit_nojoin': 'SELECT * FROM t2 WHERE b < 500 ORDER BY fk LIMIT 10',
    'u20_distinct3': 'SELECT DISTINCT t1.a, t1.b FROM t1 JOIN t2 ON t2.fk = t1.id JOIN t3 ON t3.fk = t2.id '
                     'WHERE t3.b < 20',
    'u21_not_in': 'SELECT * FROM t1 WHERE a < 3 AND b NOT IN (SELECT b FROM t2 WHERE t2.a = 7)',
    'u22_cte_limit': 'WITH x AS MATERIALIZED (SELECT t1.a, t2.b FROM t1 JOIN t2 ON t2.fk = t1.id '
                     'WHERE t2.b < 50) SELECT * FROM x ORDER BY a LIMIT 3',
    # initPlans: their cost is charged to the final relation's paths
    'u23_initplan': 'SELECT t1.id, t2.b FROM t1 JOIN t2 ON t2.fk = t1.id WHERE t1.a < 10 '
                    'AND t2.b < (SELECT max(b) FROM t3 WHERE t3.a < 5)',
    'u24_initplan_limit': 'SELECT t1.a, count(*) FROM t1 JOIN t2 ON t2.fk = t1.id WHERE t1.a < 10 GROUP BY t1.a '
                          'HAVING count(*) > (SELECT count(*) / 100 FROM t3) ORDER BY t1.a LIMIT 5',
    # targets put into paths in place (apply_projection_to_path): a volatile
    # column evaluated after the sort, set-returning functions
    'u25_volatile_order': 'SELECT t1.id, t2.b + random() FROM t1 JOIN t2 ON t2.fk = t1.id WHERE t1.a < 10 '
                          'ORDER BY t1.id LIMIT 20',
    'u26_srf_order': 'SELECT t1.id, generate_series(1, t2.b % 3) FROM t1 JOIN t2 ON t2.fk = t1.id '
                     'WHERE t1.a < 10 ORDER BY t1.id',
}
# upper path types replay does not know (status unsupported where they vary)
UNKNOWN = ('GroupingSets', 'RecursiveUnion')
LIMIT_QUERIES = ('q41_order_limit', 'f4_order', 'u04_order_limit', 'u14_limit_join3', 'u16_sub_group_limit',
                 'u19_limit_nojoin', 'u22_cte_limit', 'u24_initplan_limit',
                 'u25_volatile_order')


def make_s9_queries():
    os.makedirs(S9Q, exist_ok=True)
    for f in os.listdir(S9Q):
        os.remove(os.path.join(S9Q, f))
    for name, sql in S9_QUERIES.items():
        with open(os.path.join(S9Q, name + '.sql'), 'w') as f:
            f.write(sql + '\n')


def runs(install, sets, tag, light=False):
    """[(kind, result)]: check_s8's sets of queries and the S9 queries,
    serial and forced parallel"""
    bench = LIGHT if light else None
    out = []
    for qdir, extra, t in ((bench, [], 'bench'), (EXTRA, [], 'extra'), (bench, PARALLEL, 'par'),
                           (S8Q, PWJ, 's8q'), (S8Q, PWJ + PARALLEL, 's8qpar'),
                           (S9Q, [], 's9q'), (S9Q, PARALLEL, 's9qpar')):
        out.append((t, dump(os.path.join(RESULTS, 's9_%s_%s.json' % (tag, t)), install, extra + sets, qdir)))
    return out


def upper_paths(evs):
    return [e for e in evs if e['ev'] == 'path' and e.get('upper')]


# ------------------------------------------------------------------ U2

U2_CASES = [
    # (name, template, base threshold, extra settings); the profile scales
    # the filters of t1 and t2, linear in the threshold
    ('agg_hash', 'SELECT t2.a, count(*) FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr} GROUP BY t2.a', 200, []),
    ('agg_plain', 'SELECT count(*), sum(t1.b) FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr}', 200, []),
    ('agg_having', 'SELECT t2.a, t2.c, count(*) FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr} '
                   'GROUP BY t2.a, t2.c HAVING count(*) > 2', 200, []),
    ('agg_sorted', 'SELECT t2.id, count(*) FROM t1 JOIN t2 ON t1.fk = t2.id WHERE t2.b < {thr} '
                   'GROUP BY t2.id ORDER BY t2.id', 200, []),
    ('distinct', 'SELECT DISTINCT t2.a, t2.c FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr}', 200, []),
    ('limit', 'SELECT * FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr} ORDER BY t1.id LIMIT 20', 200, []),
    ('limit_offset', 'SELECT t1.id FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr} ORDER BY t2.a '
                     'OFFSET 10 LIMIT 20', 200, []),
    ('window', 'SELECT t2.a, row_number() OVER (PARTITION BY t2.a ORDER BY t1.id) FROM t1, t2 '
               'WHERE t1.fk = t2.id AND t2.b < {thr}', 200, []),
    ('single_group', 'SELECT a, count(*) FROM t1 WHERE b < {thr} GROUP BY a', 200, []),
    ('single_distinct', 'SELECT DISTINCT c FROM t1 WHERE b < {thr}', 200, []),
    ('lockrows', 'SELECT * FROM t1 WHERE b < {thr} FOR UPDATE', 200, []),
    ('update', 'UPDATE t1 SET c = c + 1 WHERE b < {thr}', 200, []),
    ('par_agg', 'SELECT t2.a, count(*) FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr} GROUP BY t2.a', 200,
     PARALLEL),
    ('par_count', 'SELECT count(*) FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr}', 200, PARALLEL),
    ('par_distinct', 'SELECT DISTINCT t2.c FROM t2 WHERE t2.b < {thr}', 200, PARALLEL),
]


def planning(sql, pdir, install, extra=()):
    q = ''.join('SET %s;\n' % s for s in extra)
    q += ("SET enable_rows_dist=on; SET robust_search=off; SET error_profile_path='%s'; SET error_sample_count=1;\n"
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


def u2(install):
    """returns (cases, upper paths compared, of which varying, skipped for
    rows, by type, problems)"""
    trivial = os.path.join(PROFILES, 'trivial')
    cases, compared, varied, skipped, types, problems = 0, 0, 0, 0, {}, []
    for K in (0.5, 2.0, 3.0):
        pdir = os.path.join(PROFILES, 's6_scale_%g' % K)
        os.makedirs(pdir, exist_ok=True)
        for alias in ('t1', 't2', 'p'):
            with open(os.path.join(pdir, alias + '.txt'), 'w') as f:
                f.write('%r 0.2\n' % (0.2 * K))
        for name, tmpl, thr, extra in U2_CASES:
            scn = planning(tmpl.format(thr=thr), pdir, install, extra)
            stk = planning(tmpl.format(thr=int(round(thr * K))), trivial, install, extra)
            cases += 1
            b1 = {e['rels']: vec(e['rows'], 1)[0] for e in scn if e['ev'] == 'rel' and e['kind'] in ('base', 'other')}
            b2 = {e['rels']: e['rows_point'] for e in stk if e['ev'] == 'rel' and e['kind'] in ('base', 'other')}
            if b1 != b2:
                problems.append('%s K=%g: base rows differ' % (name, K))
                continue
            s1, s2 = signatures(scn), signatures(stk)
            i1 = {e['id']: e for e in scn if e['ev'] == 'path'}
            i2 = {e['id']: e for e in stk if e['ev'] == 'path'}
            n_here = 0
            for k in s1.keys() & s2.keys():
                a, b = s1[k], s2[k]
                if not a.get('upper'):
                    continue
                if a['status'] not in ('replay', 'const'):
                    problems.append('%s K=%g %s: status %s' % (name, K, a['type'], a['status']))
                    continue
                # the inputs must agree (the S5 model holds some join
                # selectivities at the point); frozen groups stay at the point
                if not all(rows_agree(i1[c], i2[d], i1, i2, 1e-6) for c, d in zip(a['children'], b['children'])) \
                        or (a.get('groups') == 'frozen' and not rows_agree(a, b, i1, i2, 1e-6)):
                    skipped += 1
                    continue
                got = [vec(a[f], 1)[0] for f in ('rows', 'startup', 'total')]
                want = [b[f + '_point'] for f in ('rows', 'startup', 'total')]
                compared += 1
                n_here += 1
                types[a['type']] = types.get(a['type'], 0) + 1
                if got[2] != a['total_point'] or got[0] != a['rows_point']:
                    varied += 1
                if not all(close(x, y, 1e-6) for x, y in zip(got, want)):
                    problems.append('%s K=%g %s/%s(%s) groups %s: replay %s, stock %s'
                                    % (name, K, a['type'], a['upper'], a['detail'], a.get('groups'), got, want))
            if n_here == 0:
                problems.append('%s K=%g: no common upper path' % (name, K))
    return cases, compared, varied, skipped, types, problems


# ------------------------------------------------------------- U4, U5

def fractional(p, fraction, s=None):
    """robustselect.c fractional_at / fractional_point"""
    if s is None:
        total, startup, rows = p['total_point'], p['startup_point'], p['rows_point']
    else:
        total, startup, rows = vec(p['total'], N)[s], vec(p['startup'], N)[s], vec(p['rows'], N)[s]
    f = fraction
    if f >= 1.0 and rows > 0:
        f = f / rows
    if f <= 0.0 or f >= 1.0:
        return total
    return startup + f * (total - startup)


def has_vectors(p):
    return any(any(x != p[f + '_point'] for x in vec(p[f], N)) for f in ('rows', 'startup', 'total'))


def check_final(where, fe, paths, cfg, st):
    bad = st['problems']
    ids = fe['cands']
    st['finals'] += 1
    if any(i not in paths for i in ids):
        bad.append(('U4', '%s: final candidate without a path event' % where))
        return
    cands = [paths[i] for i in ids]
    # paths passed through unchanged (no Limit, LockRows, ModifyTable on
    # top) keep their own parent, the scan/join or ordered relation
    if len(set(ids)) != len(ids) or any(c['partial'] for c in cands):
        bad.append(('U4', '%s: final candidates repeated or partial' % where))
    vectors = any(has_vectors(c) for c in cands)
    if vectors != fe['vectors']:
        bad.append(('U4', '%s: vectors %s, recomputed %s' % (where, fe['vectors'], vectors)))
        return
    fr, tau, oid = fe['fraction'], cfg['tau'], cfg['main']
    lam, alpha = cfg.get('lam') or 1.0, cfg.get('alpha') or 0.85
    how = objectives.loss(oid)
    F = [[fractional(c, fr, s) for s in range(N)] for c in cands]
    env = [min(f[s] for f in F) for s in range(N)]
    scores, keys = [], []
    for i, c in enumerate(cands):
        st_ = vec(c['startup'], N)
        xs = [objectives.loss_at(how, F[i][s], st_[s], env[s], tau) for s in range(N)]
        sc = fractional(c, fr) if how == 'point' else objectives.aggregate(objectives.agg(oid), xs, lam, alpha)
        scores.append(sc)
        keys.append((sc, mean(F[i]), fractional(c, fr), c['startup_point'], i))
    if scores != fe['scores']:
        bad.append(('U4', '%s: scores %s, recomputed %s' % (where, fe['scores'], scores)))
        return
    chosen = ids[min(range(len(ids)), key=lambda i: keys[i])] if vectors else -1
    if chosen != fe['chosen']:
        bad.append(('U4', '%s: chose %s, recomputed %s' % (where, fe['chosen'], chosen)))
        return
    if vectors:
        st['picked'] += 1
        st['differs'] += chosen != fe['pg']
        st['fraction'] += fr > 0
    st['final_cands'] += len(ids)


def check_upper_keep(where, k, paths, cfg, st):
    """robust_select_score_rel: all kept, ordered by the main objective
    against the cheapest of the candidates in each scenario"""
    bad = st['problems']
    ids = k['cands']
    n = len(ids)
    if any(i not in paths for i in ids):
        bad.append(('U5', '%s: candidate without a path event' % where))
        return
    cands = [paths[i] for i in ids]
    pool = cands
    if not k['partial'] and any(c['param'] is None for c in cands):
        pool = [c for c in cands if c['param'] is None]
    env = [min(vec(c['total'], N)[s] for c in pool) for s in range(N)]
    main = [objective(cfg['main'], c, env, cfg['tau'], cfg.get('lam') or 1.0, cfg.get('alpha') or 0.85)
            for c in cands]
    if main != k['main']:
        bad.append(('U5', '%s: scores %s, recomputed %s' % (where, k['main'], main)))
        return
    mt = [mean(vec(c['total'], N)) for c in cands]
    order = sorted(range(n), key=lambda i: (main[i], mt[i], cands[i]['total_point'], cands[i]['startup_point'], i))
    if [ids[i] for i in order] != k['kept'] or k['limit'] != n or k['retain'] != 0:
        bad.append(('U5', '%s: kept %s, recomputed %s' % (where, k['kept'], [ids[i] for i in order])))
        return
    st['upper_keeps'] += 1
    st['upper_gathers'] += sum(1 for c in cands if c['type'] in ('Gather', 'GatherMerge'))
    st['upper_single'] += ' ' not in (k['rels'] or '')


def check_planning(where, evs, cfg, st, on_final=None):
    paths = {}
    for e in evs:
        if e['ev'] == 'path':
            paths[e['id']] = e
            if e.get('upper'):
                key = (e['type'], e['status'], e.get('groups'))
                st['upper'][key] = st['upper'].get(key, 0) + 1
                if e['status'] in ('mismatch', 'nocontext') or \
                        (e['status'] == 'unsupported' and e['type'] not in UNKNOWN):
                    st['problems'].append(('U3', '%s: %s %s %s' % (where, e['type'], e['upper'], e['status'])))
        elif e['ev'] == 'charge':
            # SS_charge_for_initplans: the initPlans' cost added in place
            for i in e['ids']:
                if i not in paths:
                    st['problems'].append(('U4', '%s: charged path %d without a path event' % (where, i)))
                    continue
                p = dict(paths[i])
                c = e['cost']
                p['startup_point'] += c
                p['total_point'] += c
                p['startup'] = [x + c for x in vec(p['startup'], N)]
                p['total'] = [x + c for x in vec(p['total'], N)]
                paths[i] = p
            st['charges'] += 1
        elif e['ev'] == 'project':
            # apply_projection_to_path: a target put into the path in place
            i = e['id']
            if i not in paths:
                st['problems'].append(('U4', '%s: projected path %d without a path event' % (where, i)))
                continue
            p = dict(paths[i])
            ds, dt = e['startup'], e['per_tuple']
            rows = vec(p['rows'], N)
            p['startup'] = [x + ds for x in vec(p['startup'], N)]
            p['total'] = [x + (ds + dt * r) for x, r in zip(vec(p['total'], N), rows)]
            p['startup_point'] += ds
            p['total_point'] += ds + dt * p['rows_point']
            paths[i] = p
            st['projects'] += 1
        elif e['ev'] == 'final':
            check_final(where, e, paths, cfg, st)
            if on_final:
                on_final(e, paths)
        elif e['ev'] == 'keep' and e['pass'] == 'upper':
            check_upper_keep('%s %s%s' % (where, e['rels'], ' partial' if e['partial'] else ''), e, paths, cfg, st)


def new_stats():
    return dict(problems=[], upper={}, finals=0, final_cands=0, picked=0, differs=0, fraction=0,
                upper_keeps=0, upper_gathers=0, upper_single=0, charges=0, projects=0)


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    ap.add_argument('--light', action='store_true', help='leave q12_clique8 out of the lognormal runs')
    ap.add_argument('--skip-s8', action='store_true')
    ap.add_argument('--time-only', action='store_true', help='U8 only')
    a = ap.parse_args()
    make_profile_dirs()
    make_queries()
    make_s9_queries()
    if a.time_only:
        u8(a.install)
        return
    allok = True
    common.stop()

    # U1
    off = runs(a.install, ['enable_rows_dist=off'], 'off')
    bad, nplan, nup, nfinal, viol = [], 0, 0, 0, []
    for prof in ('trivial', 'ones'):
        ident = s8_sets(prof, main=3, rlimit=0, limit=BIG, final=BIG, pairing='native', dominance=True)
        for (t, ref), (_, res) in zip(off, runs(a.install, ident, 'ident_' + prof)):
            bad += [(prof, t) + b for b in same_as_stock(ref, res)]
            nplan += len(ref['queries'])
            for q, evs in all_events(res).items():
                for e in evs:
                    if e['ev'] == 'path' and e.get('upper'):
                        nup += 1
                        if e['status'] not in ('replay', 'const') and \
                                not (e['status'] == 'unsupported' and e['type'] in UNKNOWN):
                            viol.append('%s %s %s %s' % (t, q, e['type'], e['status']))
                        elif has_vectors(e):
                            viol.append('%s %s %s: vectors off the point' % (t, q, e['type']))
                    elif e['ev'] == 'final':
                        nfinal += 1
                        if e['vectors'] or e['chosen'] != -1:
                            viol.append('%s %s: final pick with vectors' % (t, q))
    allok &= report('U1', not bad and not viol and nup > 0,
                    '%d plannings identical to stock; %d upper paths with their point in every scenario; '
                    '%d final picks, PostgreSQL\'s' % (nplan, nup, nfinal)
                    if not bad and not viol else '%s %s' % (bad[:5], viol[:5]))

    # U2
    cases, compared, varied, skipped, types, problems = u2(a.install)
    allok &= report('U2', not problems and compared > 0 and varied > 0,
                    '%d cases, %d upper paths equal to stock PostgreSQL on the scaled query (%d of them off '
                    'their point; %d skipped: frozen groups or inputs whose rows differ); %s'
                    % (cases, compared, varied, skipped, ', '.join('%s %d' % kv for kv in sorted(types.items())))
                    if not problems else '%d problems: %s' % (len(problems), '; '.join(problems[:5])))

    # U3 .. U6
    configs = [
        ('A', dict(main=0, retain=0, limit=1, rlimit=1, final=1, tau=0.2), a.light),
        ('K4', dict(main=0, retain=0, limit=1, rlimit=1, final=4, tau=0.2), a.light),
        ('C', dict(main=2, retain=5, limit=3, rlimit=1, final=2, tau=0.2), True),
        ('D', dict(main=4, retain=1, limit=1, rlimit=2, final=1, tau=0.2), True),
        ('B', dict(main=1, retain=3, limit=2, rlimit=2, final=3, tau=0.5), True),
    ]
    total = new_stats()
    results = {}
    for tag, cfg, light in configs:
        st = new_stats()
        res = runs(a.install, s8_sets('lognormal', **cfg), 'logn_' + tag, light=light)
        results[tag] = res
        for t, r in res:
            for q, evs in all_events(r).items():
                if 'error' in r['queries'][q]:
                    st['problems'].append(('U3', '%s %s: %s' % (t, q, r['queries'][q]['error'][-200:])))
                    continue
                check_planning('%s %s %s' % (tag, t, q), evs, cfg, st)
        print('       %s main %d final %d: %d final picks (%d with vectors, %d differing from PostgreSQL\'s, '
              '%d fractional, %d with initPlans charged), %d scan/join scorings (%d Gather candidates, '
              '%d single relations)'
              % (tag, cfg['main'], cfg['final'], st['finals'], st['picked'], st['differs'], st['fraction'],
                 st['charges'], st['upper_keeps'], st['upper_gathers'], st['upper_single']))
        for k, v in st.items():
            if k == 'problems':
                total['problems'] += [(g, '%s: %s' % (tag, p)) for g, p in v]
            elif k == 'upper':
                for kk, vv in v.items():
                    total['upper'][kk] = total['upper'].get(kk, 0) + vv
            else:
                total[k] += v
    probs = lambda g: [p for x, p in total['problems'] if x == g]
    groups = {}
    for (typ, status, g), n in total['upper'].items():
        if g:
            groups[g] = groups.get(g, 0) + n
    statuses = {}
    for (typ, status, g), n in total['upper'].items():
        statuses[status] = statuses.get(status, 0) + n
    allok &= report('U3', not probs('U3') and statuses.get('replay', 0) > 0,
                    'upper paths: %s; groups from %s' % (statuses, groups)
                    if not probs('U3') else '; '.join(probs('U3')[:5]))
    allok &= report('U4', not probs('U4') and total['picked'] > 0 and total['charges'] > 0 and total['projects'] > 0,
                    '%d final picks of %d candidates recomputed exactly (%d with vectors, %d fractional, '
                    '%d final relations charged for initPlans, %d targets put into paths in place); '
                    '%d differ from PostgreSQL\'s'
                    % (total['finals'], total['final_cands'], total['picked'], total['fraction'],
                       total['charges'], total['projects'], total['differs'])
                    if not probs('U4') else '; '.join(probs('U4')[:5]))
    allok &= report('U5', not probs('U5') and total['upper_keeps'] > 0 and total['upper_gathers'] > 0
                    and total['upper_single'] > 0,
                    '%d scan/join relation scorings recomputed (%d Gather candidates, %d single relations)'
                    % (total['upper_keeps'], total['upper_gathers'], total['upper_single'])
                    if not probs('U5') else '; '.join(probs('U5')[:5]))

    # U6
    changed, lim = [], []
    for (t, r1), (_, r4) in zip(results['A'], results['K4']):
        for q in r1['queries']:
            p1, p4 = r1['queries'][q].get('plan'), r4['queries'][q].get('plan')
            if p1 and p4 and compare.compare_plans(p1, p4, 1e-9)[0] == 'PLAN-DIFF':
                changed.append('%s %s' % (t, q))
                if q in LIMIT_QUERIES:
                    lim.append('%s %s' % (t, q))
    report('U6', True, 'final relation keeping 4 paths instead of 1 changes %d plans (%d with LIMIT): %s'
           % (len(changed), len(lim), ', '.join(changed[:12])))

    # U7
    if not a.skip_s8:
        common.stop()
        r = subprocess.run([sys.executable, os.path.join(common.TOOLS, 'check_s8.py'), '--install', a.install]
                           + (['--light'] if a.light else []), capture_output=True, text=True)
        allok &= report('U7', r.returncode == 0, 'check_s8: ' + ' | '.join(
            l.split()[0] + ' ' + l.split()[1] for l in r.stdout.splitlines() if l[:1] == 'G'))
        if r.returncode != 0:
            print(r.stdout[-3000:])

    u8(a.install)
    sys.exit(0 if allok else 1)


def u8(install):
    common.stop()
    tm = {}
    for tag, sets in (('stock', ['enable_rows_dist=off']),
                      ('s9', s8_sets('lognormal', debug=False)),
                      ('s9_k4', s8_sets('lognormal', final=4, debug=False)),
                      ('s9_noenv', s8_sets('lognormal', main=3, retain=2, debug=False)),
                      ('s9_triv', s8_sets('trivial', debug=False))):
        tm[tag] = {}
        for qdir in (None, S9Q):
            res = dump(os.path.join(RESULTS, 's9_time_%s.json' % tag), install, sets, qdir, repeat=5)
            tm[tag].update({q: v['planning_ms'] for q, v in res['queries'].items() if v.get('planning_ms')})
    msg = []
    for tag in ('s9', 's9_k4', 's9_noenv', 's9_triv'):
        for what, sel in (('bench', lambda q: q[0] == 'q'), ('s9q', lambda q: q[0] == 'u')):
            r = {q: tm[tag][q] / tm['stock'][q] for q in tm['stock'] if q in tm[tag] and sel(q)}
            w = max(r, key=r.get)
            msg.append('%s %s %.2f (max %.1f %s)' % (tag, what, gm(r.values()), r[w], w))
    report('U8', True, 'planning time / stock, geometric mean: ' + '; '.join(msg))
    common.stop()


if __name__ == '__main__':
    main()
