#!/usr/bin/env python3
"""
S5 gate: row vectors of join relations and parameterized joins follow
robustrows.c exactly, and agree with PostgreSQL itself.

    check_s5.py [--install v2-release]

  J1  robust on, any profile: plans identical to stock
  J2  trivial and all-ones profiles: every join rel rows[s] == rows_point
      (first pair) or pair_point (later pairs), every join ppi rows[s] ==
      rows_point, every replayed selectivity == the point selectivity
  J3  shift / two-point / lognormal profiles: every join rel (all pairs)
      and join ppi vector bit-identical to a Python recomputation; the
      input vectors equal the vectors of the input rels / ppis
  J4  inner-join queries: the vector of a join rel built from another pair
      differs from the first pair's by rounding only (reported against
      PostgreSQL's own pair dependence)
  J5  bounds per join type (left >= outer, full >= max, semi/anti <= outer,
      inner <= outer * inner) on every event
  J6  replay against PostgreSQL: a profile that scales one filter's
      selectivity by K gives, for inner, left, full, semi and anti joins,
      exactly the rows stock PostgreSQL estimates for the query whose
      filter really selects K times as much
  J7  S4 gates (and through them S3, S2, S1) still pass
  J8  planning time, robust on vs off (informational)
"""
import argparse
import json
import math
import os
import subprocess
import sys
from fractions import Fraction

import common
from check_s4 import (PROFILES, EXTRA, RESULTS, Reference, all_events, clamp_row_est, dump,
                      make_profile_dirs, robust_sets, same_as_stock, vec, report)
from robdp_dump import parse

FUSED_ANTI = True       # clang contracts o * (1.0 - a * b) into an FMA


def one_minus_prod(a, b):
    if FUSED_ANTI:
        return float(Fraction(1) - Fraction(a) * Fraction(b))
    return 1.0 - a * b


def join_rows_at(jt, o, i, sel, qms, s):
    mj = mp = 1.0
    for pushed, m in qms:
        if pushed:
            mp = mp * m[s]
        else:
            mj = mj * m[s]
    fkm = sel[0] * mj
    ps = sel[2] * mp
    if ps > 1.0:
        ps = 1.0
    j = sel[1]
    if jt in ('inner', 'left', 'full'):
        n = o * i * fkm * j
        cap = o * i
        if n > cap:
            n = cap
        if jt in ('left', 'full') and n < o:
            n = o
        if jt == 'full' and n < i:
            n = i
        if jt != 'inner':
            n *= ps
    elif jt == 'semi':
        n = o * fkm * j
        if n > o:
            n = o
    elif jt == 'anti':
        n = o * one_minus_prod(fkm, j)
        if n < 0.0:
            n = 0.0
        n *= ps
    else:
        raise ValueError(jt)
    return clamp_row_est(n)


def check_joins(events, ref, N):
    rows_of, ppi_of = {}, {}
    problems, n = [], 0
    for e in events:
        if e['ev'] == 'rel':
            if e['kind'] != 'join':
                rows_of[e['rels']] = vec(e['rows'], N)
                continue
            n += 1
            got = vec(e['rows'], N)
            o, i = vec(e['outer_rows'], N), vec(e['inner_rows'], N)
            if o != rows_of.get(e['pair'][0]) or i != rows_of.get(e['pair'][1]):
                problems.append('join %s: inputs differ from the rel vectors' % e['rels'])
            qms = [(q.get('pushed', False), ref.querylet(q)[1]) for q in e['querylets']]
            sels = e.get('sels') or [e['sel']] * N
            want = [join_rows_at(e['jointype'], o[s], i[s], sels[s], qms, s) for s in range(N)]
            if got != want:
                problems.append('join %s %s pair %s: C %s, Python %s' % (e['jointype'], e['rels'], e['pair'],
                                                                         got[:3], want[:3]))
            if e['first']:
                rows_of[e['rels']] = got
        elif e['ev'] == 'ppi':
            got = vec(e['rows'], N)
            if e['kind'] == 'join':
                n += 1
                ins = []
                for k in range(2):
                    src = e['pair_src'][k]
                    if src == 'rel':
                        ins.append(rows_of.get(e['pair'][k]))
                    elif src == 'ppi':
                        ins.append(ppi_of.get((e['pair'][k], e['pair_param'][k])))
                    else:
                        ins.append(None)
                o, i = vec(e['outer_rows'], N), vec(e['inner_rows'], N)
                if (ins[0] is not None and ins[0] != o) or (ins[1] is not None and ins[1] != i):
                    problems.append('join ppi %s/%s: inputs differ' % (e['rels'], e['outer']))
                qms = [(q.get('pushed', False), ref.querylet(q)[1]) for q in e['querylets']]
                sels = e.get('sels') or [e['sel']] * N
                cap = rows_of[e['rels']]
                want = [min(join_rows_at(e['jointype'], o[s], i[s], sels[s], qms, s), cap[s]) for s in range(N)]
                if got != want:
                    problems.append('join ppi %s/%s: C %s, Python %s' % (e['rels'], e['outer'], got[:3], want[:3]))
            ppi_of[(e['rels'], e['outer'])] = got
    return n, problems


def bounds(events, N):
    bad = []
    for e in events:
        if e['ev'] != 'rel' or e['kind'] != 'join':
            continue
        r, o, i = vec(e['rows'], N), vec(e['outer_rows'], N), vec(e['inner_rows'], N)
        no_pushed = e['sel'][2] == 1 and not any(q.get('pushed') for q in e['querylets'])
        for s in range(N):
            jt, ok = e['jointype'], True
            if jt == 'inner':
                ok = r[s] <= max(1.0, o[s] * i[s])
            elif jt == 'left' and no_pushed:
                ok = r[s] >= o[s]
            elif jt == 'full' and no_pushed:
                ok = r[s] >= max(o[s], i[s])
            elif jt in ('semi', 'anti'):
                ok = r[s] <= max(1.0, o[s])
            if not ok:
                bad.append('%s %s s=%d: rows %g, outer %g, inner %g' % (jt, e['rels'], s, r[s], o[s], i[s]))
                break
    return bad


EC_QUERIES = {'q10_clique4', 'q11_clique6', 'q12_clique8', 'q20_star'}


def pair_dependence(events, N):
    """
    Compare each later pair's vector with the first pair's.  Each
    clamp_row_est rounding changes a value by at most 0.5, i.e. by a
    relative 0.5 / rows, so two trees can differ by at most the sum over
    both trees' join nodes; a sample where some node was clamped to 1 is
    skipped.  Returns (pairs*samples, violations, skipped, worst ratio,
    relative differences) or None if the query has non-inner joins.
    """
    joins = [e for e in events if e['ev'] == 'rel' and e.get('kind') == 'join']
    if not joins or any(e['jointype'] != 'inner' for e in joins):
        return None
    err, clamped = {}, {}
    for e in events:
        if e['ev'] == 'rel' and e.get('kind') != 'join':
            err[e['rels']] = [0.0] * N
            clamped[e['rels']] = [False] * N
    first = {}
    for e in joins:
        r = vec(e['rows'], N)
        o, i = e['pair']
        er = [err[o][s] + err[i][s] + 0.5 / r[s] for s in range(N)]
        cl = [clamped[o][s] or clamped[i][s] or r[s] <= 1.0 for s in range(N)]
        if e['first']:
            first[e['rels']] = r
            err[e['rels']], clamped[e['rels']] = er, cl
        else:
            e['_err'], e['_cl'] = er, cl
    n = viol = skip = 0
    worst = 0.0
    rels = []
    for e in joins:
        if e['first']:
            continue
        a, b = first[e['rels']], vec(e['rows'], N)
        for s in range(N):
            n += 1
            if e['_cl'][s] or clamped[e['rels']][s]:
                skip += 1
                continue
            bound = (e['_err'][s] + err[e['rels']][s]) * max(a[s], b[s]) + 1e-9 * max(a[s], b[s])
            d = abs(a[s] - b[s])
            rels.append(d / max(a[s], b[s]))
            worst = max(worst, d / bound)
            if d > bound:
                viol += 1
    return n, viol, skip, worst, rels


J6_CASES = [
    # (join type, query template, profile alias, base threshold, sel_est of the filter is thr/1000 or thr/100)
    ('inner', 'SELECT * FROM t1, t2 WHERE t1.fk = t2.id AND t2.b < {thr}', 't2', 200),
    ('left', 'SELECT * FROM (SELECT * FROM t1 WHERE b < {thr}) t1 LEFT JOIN t2 ON t2.fk = t1.id', 't1', 200),
    ('full', 'SELECT count(*) FROM (SELECT * FROM t1 WHERE b < {thr}) t1 FULL JOIN t2 ON t2.fk = t1.id', 't1', 200),
    ('semi', 'SELECT count(*) FROM t1 WHERE EXISTS (SELECT 1 FROM t2 WHERE t2.fk = t1.id AND t2.b < {thr})', 't2', 200),
    ('anti', 'SELECT count(*) FROM t1 WHERE NOT EXISTS (SELECT 1 FROM t2 WHERE t2.fk = t1.id AND t2.b < {thr})', 't2', 200),
    ('semi', 'SELECT count(*) FROM t2 WHERE EXISTS (SELECT 1 FROM t1 WHERE t1.id = t2.fk AND t1.b < {thr})', 't1', 100),
    ('anti', 'SELECT count(*) FROM t2 WHERE NOT EXISTS (SELECT 1 FROM t1 WHERE t1.id = t2.fk AND t1.b < {thr})', 't1', 100),
]


def plan_rows(plan):
    """rows of the topmost join node"""
    def walk(p):
        if 'Join' in p['Node Type'] or p['Node Type'] == 'Nested Loop':
            return p['Plan Rows']
        for c in p.get('Plans', []):
            r = walk(c)
            if r is not None:
                return r
        return None
    return walk(plan[0]['Plan'])


def j6(install):
    """replay against PostgreSQL; returns (checked, problems, lines)"""
    lines, problems, checked = [], [], 0
    for K in (0.5, 2.0, 3.0):
        pdir = os.path.join(PROFILES, 's5_scale_%g' % K)
        os.makedirs(pdir, exist_ok=True)
        for alias in ('t1', 't2'):
            with open(os.path.join(pdir, alias + '.txt'), 'w') as f:
                f.write('%r 0.2\n' % (0.2 * K))
        for jt, tmpl, alias, thr in J6_CASES:
            q_scn = tmpl.format(thr=thr)
            q_stock = tmpl.format(thr=int(round(thr * K)))
            sql = ("SET enable_rows_dist=on; SET error_profile_path='%s'; SET error_sample_count=1;\n"
                   "SET robust_debug_dump=on; SET client_min_messages=log;\nEXPLAIN %s;\n"
                   "RESET robust_debug_dump; RESET client_min_messages; RESET enable_rows_dist;\n"
                   "EXPLAIN (FORMAT JSON) %s;\nEXPLAIN (FORMAT JSON) SELECT * FROM %s WHERE b < %d;\n"
                   % (pdir, q_scn, q_stock, alias, int(round(thr * K))))
            out, err = common.psql(sql, install, on_error_stop=False)
            ev = parse(err.splitlines())
            base = [e for e in ev.get('rel', []) if e['kind'] in ('base', 'other') and e['querylets']]
            joins = [e for e in ev.get('rel', []) if e['kind'] == 'join' and e['first']]
            # the two JSON plans are printed one after the other
            text = out.strip()
            plans = []
            depth, start = 0, None
            for k, ch in enumerate(text):
                if ch == '[' and depth == 0:
                    start = k
                if ch in '[{':
                    depth += 1
                elif ch in ']}':
                    depth -= 1
                    if depth == 0 and start is not None:
                        plans.append(json.loads(text[start:k + 1]))
                        start = None
            stock_join = plan_rows(plans[0])
            stock_base = plans[1][0]['Plan']['Plan Rows']
            scn_base = vec(base[0]['rows'], 1)[0] if base else None
            scn_join = vec(joins[-1]['rows'], 1)[0] if joins else None
            checked += 1
            line = '%s K=%g: base %s vs stock %s; join %s vs stock %s' % (jt, K, scn_base, stock_base,
                                                                         scn_join, stock_join)
            lines.append(line)
            if scn_base == stock_base and scn_join != stock_join:
                problems.append(line)
            if scn_base != stock_base and abs(scn_join - stock_join) > 0.01 * stock_join + 1:
                problems.append(line + ' (base rows differ too)')
    return checked, problems, lines


def main():
    global FUSED_ANTI
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    a = ap.parse_args()
    make_profile_dirs()
    stock = json.load(open(os.path.join(RESULTS, 'stock_v2rel.json')))
    P = lambda name: os.path.join(PROFILES, name)
    allok = True

    # J1
    bad_all, nq = [], 0
    for d in ('trivial', 's4_two', 'lognormal'):
        res = dump(os.path.join(RESULTS, 's5_j1.json'), a.install, robust_sets(P(d)))
        bad = same_as_stock(stock, res)
        nq += len(stock['queries'])
        bad_all += [(d,) + b for b in bad]
    allok &= report('J1', not bad_all, '%d plannings (dump on, all pairs evaluated) identical to stock' % nq
                    if not bad_all else str(bad_all[:5]))

    # J2
    problems, n_ev, n_rep = [], 0, 0
    for d in ('trivial', 'ones'):
        for qdir in (None, EXTRA):
            res = dump(os.path.join(RESULTS, 's5_j2.json'), a.install, robust_sets(P(d), bins=3), qdir)
            for name, evs in all_events(res).items():
                for e in evs:
                    if e.get('kind') != 'join':
                        continue
                    n_ev += 1
                    point = e['rows_point'] if e['ev'] == 'ppi' or e['first'] else e['pair_point']
                    if any(v != point for v in vec(e['rows'], 20)):
                        problems.append('%s %s %s %s' % (d, name, e['ev'], e['rels']))
                    if 'sels' in e:
                        n_rep += 1
                        if any(sl != e['sel'] for sl in e['sels']):
                            problems.append('%s %s %s: replayed sel differs' % (d, name, e['rels']))
    allok &= report('J2', not problems and n_ev > 0,
                    '%d join rel/ppi events: rows[s] == point; %d replays reproduce the point selectivity'
                    % (n_ev, n_rep) if not problems else '; '.join(problems[:4]))

    # J3, J4, J5
    configs = [('s4_shift4', 0.0, 42, 2, 20), ('s4_two', 0.0, 42, 3, 20), ('lognormal', 0.0, 42, 2, 20),
               ('lognormal', -1.0, 7, 4, 50), ('lognormal', 0.3, 0, 1, 1)]
    for attempt in (True, False):
        FUSED_ANTI = attempt
        problems, counts, varying, bnd = [], 0, 0, []
        pair_stats, ec_pairs = [], []
        for d, bw, seed, nb, N in configs:
            ref = Reference(P(d), N, bw, seed, nb)
            for qdir in (None, EXTRA):
                res = dump(os.path.join(RESULTS, 's5_j3.json'), a.install, robust_sets(P(d), N, bw, seed, nb), qdir)
                for name, evs in all_events(res).items():
                    try:
                        c, p = check_joins(evs, ref, N)
                    except (KeyError, AssertionError) as ex:
                        c, p = 0, ['%s: reference failed: %r' % (name, ex)]
                    counts += c
                    problems += ['%s %s bw=%g: %s' % (d, name, bw, x) for x in p]
                    bnd += ['%s %s: %s' % (d, name, x) for x in bounds(evs, N)]
                    varying += sum(1 for e in evs if e.get('kind') == 'join' and isinstance(e['rows'], list)
                                   and len(set(e['rows'])) > 1)
                    # J4: pair dependence, against a rounding bound
                    pd = pair_dependence(evs, N)
                    if pd is not None:
                        (ec_pairs if name in EC_QUERIES else pair_stats).append(pd)
        if not problems:
            break
    allok &= report('J3', not problems and varying > 0,
                    '%d join rel/ppi events bit-identical over %d configurations (%d vectors vary; anti '
                    'join formula %s)' % (counts, len(configs), varying, 'fused as in stock' if FUSED_ANTI
                                          else 'unfused') if not problems else '; '.join(problems[:4]))
    n_pairs = sum(p[0] for p in pair_stats)
    n_viol = sum(p[1] for p in pair_stats)
    n_skip = sum(p[2] for p in pair_stats)
    worst = max((p[3] for p in pair_stats), default=0)
    ec_n = sum(p[0] for p in ec_pairs)
    ec_viol = sum(p[1] for p in ec_pairs)
    ec_med = sorted(x for p in ec_pairs for x in p[4])
    allok &= report('J4', n_pairs > 0 and n_viol == 0,
                    '%d (pair, s) of later pairs in queries without multi-member equivalence classes: '
                    '%d outside the rounding bound (%d skipped: clamped to 1); worst |diff| / bound %.2f.  '
                    'Informational, equivalence-class queries %s: %d of %d outside the bound, median '
                    'relative difference %.2f'
                    % (n_pairs, n_viol, n_skip, worst, '/'.join(sorted(EC_QUERIES)), ec_viol, ec_n,
                       ec_med[len(ec_med) // 2] if ec_med else 0))
    allok &= report('J5', not bnd, 'join-type bounds hold on every event' if not bnd else '; '.join(bnd[:3]))

    # J6
    common.start(a.install)
    checked, probs, lines = j6(a.install)
    allok &= report('J6', not probs, '%d replay cases equal to PostgreSQL' % checked if not probs
                    else '; '.join(probs[:3]))
    for l in lines:
        print('       ' + l)

    # J7
    common.stop()
    r = subprocess.run([sys.executable, os.path.join(common.TOOLS, 'check_s4.py'), '--install', a.install],
                       capture_output=True, text=True)
    allok &= report('J7', r.returncode == 0, 'check_s4: ' + ' | '.join(
        l.split()[0] + ' ' + l.split()[1] for l in r.stdout.splitlines() if l[:1] == 'R'))

    # J8
    off = dump(os.path.join(RESULTS, 's5_off.json'), a.install, ['enable_rows_dist=off'], repeat=7)
    on = dump(os.path.join(RESULTS, 's5_on.json'), a.install, robust_sets(P('lognormal'), debug=False), repeat=7)
    ratios = {qn: on['queries'][qn]['planning_ms'] / off['queries'][qn]['planning_ms'] for qn in off['queries']
              if off['queries'][qn].get('planning_ms') and on['queries'][qn].get('planning_ms')}
    gm = math.exp(sum(math.log(x) for x in ratios.values()) / len(ratios))
    worst = max(ratios, key=ratios.get)
    report('J8', True, 'planning time on (lognormal, N=20) / off: geometric mean %.3f, max %.2f (%s)'
           % (gm, ratios[worst], worst))
    common.stop()
    sys.exit(0 if allok else 1)


if __name__ == '__main__':
    main()
