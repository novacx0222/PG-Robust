#!/usr/bin/env python3
"""
S8 gate: the selection of the main pass (optimizer/robustselect.h): the
candidates of each relation are collected, and the main objective's
add_path_limit best and the retain strategy's retain_path_limit best of the
others are kept, against the relation's envelope for its complete and for
its partial paths; with robust_join_pairing = retained the joins are built
from every path the inputs keep.

    check_s8.py [--install v2-release] [--light] [--skip-s7]

  G1  stock identity: trivial and all-ones profiles, objective 3 (expected
      total), no retained paths, robust_dominance on, limits beyond any
      pathlist (robust_final_path_limit too), native pairing: plans
      identical to stock PostgreSQL (benchmark, extra, forced parallel, S8
      queries with partitionwise joins); every keep event keeps all its
      candidates.  With the default final limit (1, v20) the plans that
      differ are reported
  G2  selection recomputed: lognormal, five settings of the objectives,
      limits, tau and pairing; for every keep event the scores of the main
      objective and the retain strategy and the paths kept (and why) are
      recomputed from the candidates' cost vectors and the envelope, exactly
  G3  pathlists: after each selection the relation's pathlist and
      partial_pathlist are the kept paths in the main objective's order, its
      cheapest total path the first unparameterized one; every complete
      selection keeps an unparameterized path when it has one (P2), and some
      need it (f5, f6: an outer join makes the inner join rel's best paths
      parameterized)
  G4  Gather and partition children: every Gather / Gather Merge candidate
      of a selected relation is built on a partial path its selection kept
      (N8); the child join relations of partitionwise joins are selected,
      against their own envelope (N9)
  G5  settings: objectives above 5 are rejected; an objective that needs the
      envelope with robust_envelope off is an error; with objectives 2 to 5
      no envelope round runs; the level-1 relations are scored, not pruned
  G6  retained vs native pairing, objective 0 (informational): objective
      value and expected total cost of the final relation's path, and the
      kept / C* distribution of join relations (complete and partial
      paths); base relations: kept == C* (to 1e-9)
  G7  check_s7 (robust_select off) still passes, and through it S6..S1
  G8  planning time / stock (informational)
"""
import argparse
import json
import math
import os
import subprocess
import sys

import common
import objectives
from check_s4 import PROFILES, EXTRA, RESULTS, all_events, dump, make_profile_dirs, robust_sets, \
    same_as_stock, vec, report
from check_s6 import PARALLEL
from check_s7 import gm

N = 20
S8Q = os.path.join(common.ROOT, 'data', 's8_queries')
S8_QUERIES = {
    # partitionwise joins (enable_partitionwise_join on): child join rels
    'f1_pwj': 'SELECT count(*) FROM p x JOIN p y ON x.id = y.id WHERE x.v < 10',
    'f2_pwj3': 'SELECT count(*) FROM p x JOIN p y ON x.id = y.id JOIN t1 ON t1.id = x.v + 1 WHERE y.v < 5',
    # parameterized join rels (t7 t8 by t1)
    'f3_param3': 'SELECT * FROM t1 JOIN t8 ON t8.fk = t1.id JOIN t7 ON t7.fk = t8.id WHERE t1.a = 3 AND t1.b = 3',
    # an outer join fixes the order: the inner join rel gets paths
    # parameterized by t1, which every objective puts first (P2)
    'f5_param_outer': 'SELECT * FROM t1 LEFT JOIN (t8 JOIN t7 ON t7.fk = t8.id) ON t8.fk = t1.id '
                      'WHERE t1.a = 3 AND t1.b = 3',
    'f6_param_outer2': 'SELECT count(*) FROM t1 LEFT JOIN (t2 JOIN t3 ON t3.id = t2.fk) ON t2.fk = t1.id '
                       'WHERE t1.a < 2',
    # a sorted path of the final relation wins in stock PostgreSQL
    'f4_order': 'SELECT t1.id, t2.b FROM t1 JOIN t2 ON t2.fk = t1.id WHERE t1.a < 10 ORDER BY t1.id LIMIT 50',
}
PWJ = ['enable_partitionwise_join=on']
HEAVY = ('q12_clique8',)
LIGHT = os.path.join(common.ROOT, 'data', 's8_light')
BIG = 1000000


def make_queries():
    for d, qs in ((S8Q, S8_QUERIES), (LIGHT, None)):
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
    for name, sql in S8_QUERIES.items():
        with open(os.path.join(S8Q, name + '.sql'), 'w') as f:
            f.write(sql + '\n')
    src = os.path.join(common.TOOLS, 'queries')
    for f in os.listdir(src):
        if f.endswith('.sql') and f[:-4] not in HEAVY:
            with open(os.path.join(src, f)) as a, open(os.path.join(LIGHT, f), 'w') as b:
                b.write(a.read())


def s8_sets(prof, main=0, retain=0, limit=1, rlimit=1, final=1, pairing='retained', dominance=False,
            tau=0.2, envelope=True, debug=True, lam=None, alpha=None):
    more = [] if lam is None else ['robust_penalty_lambda=%r' % lam]
    more += [] if alpha is None else ['robust_quantile_alpha=%r' % alpha]
    return robust_sets(os.path.join(PROFILES, prof), search=True, debug=debug, more=[
        'robust_select=on', 'main_objective_id=%d' % main, 'retain_strategy_id=%d' % retain,
        'add_path_limit=%d' % limit, 'retain_path_limit=%d' % rlimit, 'robust_final_path_limit=%d' % final,
        'robust_join_pairing=' + pairing, 'robust_dominance=' + ('on' if dominance else 'off'),
        'robust_penalty_tau=%r' % tau, 'robust_envelope=' + ('on' if envelope else 'off')] + more)


def runs(install, sets, tag, light=False):
    """[(kind, result)]: benchmark, extra, forced parallel, S8 queries
    (partitionwise joins on), serial and parallel"""
    bench = LIGHT if light else None
    out = []
    for qdir, extra, t in ((bench, [], 'bench'), (EXTRA, [], 'extra'), (bench, PARALLEL, 'par'),
                           (S8Q, PWJ, 's8q'), (S8Q, PWJ + PARALLEL, 's8qpar')):
        out.append((t, dump(os.path.join(RESULTS, 's8_%s_%s.json' % (tag, t)), install, extra + sets, qdir)))
    return out


# ------------------------------------------------------------ objectives

def mean(v):
    s = 0.0
    for x in v:
        s += x
    return s / len(v)


def vmax(v):
    m = 0.0
    for x in v:
        if x > m:
            m = x
    return m


def objective(oid, p, env, tau, lam=1.0, alpha=0.85):
    """robustselect.c objective() (objectives.py)"""
    return objectives.objective(oid, p, env, tau, lam, alpha, N)


def needs_env(oid):
    return objectives.needs_env(oid)


# ------------------------------------------------------------- G2 .. G4

class Stats:
    def __init__(self):
        self.keeps = 0
        self.cands = 0
        self.kept = 0
        self.unparam = 0            # complete selections that needed the rescue
        self.param_kept = 0         # parameterized paths among the kept
        self.gathers = 0            # Gather candidates checked
        self.child = 0              # child join rel selections
        self.child_env = 0          # ... against an envelope
        self.child_envs = 0         # child env events
        self.noenv = 0              # selections without an envelope under an envelope objective
        self.level1 = 0             # level-1 selections (scored only)
        self.pathlists = 0
        self.dv_picks = 0           # paths a diversification function retained
        self.problems = []


def is_child(rels):
    return ':' in rels


def check_query(name, evs, cfg, st):
    """G2..G4 for one planning"""
    paths, env = {}, {}
    last_keep = {}                  # (rels, partial) -> keep event, the latest
    bad = st.problems
    for e in evs:
        ev = e['ev']
        if ev == 'path':
            paths[e['id']] = e
        elif ev == 'env':
            env[e['rels']] = e
            st.child_envs += e.get('child', False)
        elif ev == 'keep' and e['pass'] == 'main':
            check_keep(name, e, paths, env, cfg, st)
            last_keep[(e['rels'], e['partial'])] = e
        elif ev == 'pathlist' and e['pass'] == 'main':
            check_pathlist(name, e, paths, last_keep, st)


def check_keep(name, k, paths, env, cfg, st):
    bad = st.problems
    where = '%s %s%s' % (name, k['rels'], ' partial' if k['partial'] else '')
    ids = k['cands']
    n = len(ids)
    st.keeps += 1
    st.cands += n
    if -1 in ids or any(i not in paths for i in ids):
        bad.append(('G2', '%s: candidate without vectors or path event' % where))
        return
    cands = [paths[i] for i in ids]
    limit, retain = k['limit'], k['retain']
    if (limit, retain) == (n, 0) and n not in (cfg['limit'], cfg['final']):
        st.level1 += 1
    elif (limit, retain) not in ((cfg['limit'], cfg['rlimit']), (cfg['final'], 0), (n, 0)):
        bad.append(('G2', '%s: limit %d retain %d' % (where, limit, retain)))
    # envelope
    ev = None
    if k['envelope']:
        e = env.get(k['rels'])
        if e is None:
            bad.append(('G2', '%s: envelope used, no env event' % where))
            return
        ev = vec(e['partial'] if k['partial'] else e['cost'], N)
    elif needs_env(cfg['main']) or (retain > 0 and needs_env(cfg['retain'])):
        st.noenv += 1
    if is_child(k['rels']) and len(k['rels'].split()) > 1:
        st.child += 1
        st.child_env += k['envelope']
    # scores
    lam, alpha = cfg.get('lam') or 1.0, cfg.get('alpha') or 0.85
    main = [objective(cfg['main'], p, ev, cfg['tau'], lam, alpha) for p in cands]
    if main != k['main']:
        i = next(j for j in range(n) if main[j] != k['main'][j])
        bad.append(('G2', '%s: main score of %d: %r, recomputed %r' % (where, ids[i], k['main'][i], main[i])))
        return
    ret = None
    dv = cfg['retain'] in objectives.DIVERSIFIERS
    if retain > 0 and not dv:
        ret = [objective(cfg['retain'], p, ev, cfg['tau'], lam, alpha) for p in cands]
        if ret != k['retain_score']:
            bad.append(('G2', '%s: retain scores differ' % where))
            return
    mt = [mean(vec(p['total'], N)) for p in cands]
    pt = [(p['total_point'], p['startup_point']) for p in cands]
    order = sorted(range(n), key=lambda i: (main[i], mt[i]) + pt[i] + (i,))
    by = ['no'] * n
    for i in order[:limit]:
        by[i] = 'main'
    if retain > 0 and limit < n and dv:
        rest = [i for i in range(n) if by[i] == 'no']
        rs = k['retain_score']
        draws = [rs[i] for i in rest] if cfg['retain'] == 7 else None
        if draws is not None and not all(0.0 <= d < 1.0 for d in draws):
            bad.append(('G2', '%s: Rand draws outside [0, 1): %s' % (where, draws)))
            return
        picks = objectives.diversify(cfg['retain'], cands, rest, retain, ev, cfg, k.get('ops'), draws, N)
        for i in picks:
            by[i] = 'retain'
        if cfg['retain'] != 7:
            want = [0.0] * n
            for i in rest:
                want[i] = -1.0
            for j, i in enumerate(picks):
                want[i] = float(j)
            if want != [float(x) for x in rs]:
                bad.append(('G2', '%s: pick order %s, recomputed %s' % (where, rs, want)))
                return
        st.dv_picks += len(picks)
    elif retain > 0 and limit < n:
        rest = sorted((i for i in range(n) if by[i] == 'no'), key=lambda i: (ret[i], mt[i]) + pt[i] + (i,))
        for i in rest[:retain]:
            by[i] = 'retain'
    if not k['partial'] and not any(by[i] != 'no' and cands[i]['param'] is None for i in range(n)):
        for i in order:
            if cands[i]['param'] is None:
                by[i] = 'unparam'
                st.unparam += 1
                break
    kept = [i for i in order if by[i] != 'no']
    if [ids[i] for i in kept] != k['kept'] or [by[i] for i in kept] != k['by']:
        bad.append(('G2', '%s: kept %s %s, recomputed %s %s' % (where, k['kept'], k['by'],
                                                                 [ids[i] for i in kept], [by[i] for i in kept])))
        return
    st.kept += len(kept)
    st.param_kept += sum(1 for i in kept if cands[i]['param'] is not None)
    if not k['partial'] and any(c['param'] is None for c in cands) and \
            not any(cands[i]['param'] is None for i in kept):
        bad.append(('G3', '%s: no unparameterized path kept' % where))


def check_pathlist(name, pl, paths, last_keep, st):
    bad = st.problems
    rels = pl['rels']
    kc, kp = last_keep.get((rels, False)), last_keep.get((rels, True))
    if kc is None:
        bad.append(('G3', '%s %s: pathlist without a selection' % (name, rels)))
        return
    st.pathlists += 1
    if pl['paths'] != kc['kept']:
        bad.append(('G3', '%s %s: pathlist %s, kept %s' % (name, rels, pl['paths'], kc['kept'])))
    if pl['partial'] != (kp['kept'] if kp else []):
        bad.append(('G3', '%s %s: partial_pathlist %s, kept %s' % (name, rels, pl['partial'], kp and kp['kept'])))
    first = next((i for i in kc['kept'] if paths[i]['param'] is None), None)
    if pl['cheapest_total'] != first:
        bad.append(('G3', '%s %s: cheapest total %s, first unparameterized kept %s'
                    % (name, rels, pl['cheapest_total'], first)))
    # N8: Gather candidates are built on kept partial paths
    for i in kc['cands']:
        p = paths[i]
        if p['type'] in ('Gather', 'GatherMerge'):
            st.gathers += 1
            sub = p['children'][0] if p['children'] else None
            # generate_useful_gather_paths sorts a kept partial path for a
            # Gather Merge without offering the sort to add_partial_path
            if sub in paths and paths[sub]['type'] in ('Sort', 'IncrementalSort') and \
                    (kp is None or sub not in kp['cands']) and paths[sub]['children']:
                sub = paths[sub]['children'][0]
            if kp is None or sub not in kp['kept']:
                bad.append(('G4', '%s %s: %s %d on partial path %s, not kept' % (name, rels, p['type'], i, sub)))


# ------------------------------------------------------------------ G6

def final_keep(evs):
    """the selection of the query's final relation: the last complete one"""
    k = [e for e in evs if e['ev'] == 'keep' and e['pass'] == 'main' and not e['partial']]
    return k[-1] if k else None


def ratios(evs):
    """[(level, partial, ratio, rels, s)]: the cheapest kept path over C*"""
    paths, env, out = {}, {}, []
    for e in evs:
        if e['ev'] == 'path':
            paths[e['id']] = e
        elif e['ev'] == 'env':
            env[e['rels']] = e
        elif e['ev'] == 'pathlist' and e['pass'] == 'main' and e['rels'] in env:
            en = env[e['rels']]
            if en.get('child'):
                continue
            for partial, key in ((False, 'paths'), (True, 'partial')):
                kept = [paths[i] for i in e[key] if paths[i]['param'] is None]
                if not kept:
                    continue
                c = vec(en['partial' if partial else 'cost'], N)
                tot = [vec(p['total'], N) for p in kept]
                for s in range(N):
                    if math.isnan(c[s]) or c[s] <= 0:
                        continue
                    out.append((en['level'], partial, min(t[s] for t in tot) / c[s], e['rels'], s))
    return out


def base_ratios(evs):
    """level-1 relations: their kept paths are all their paths, so the
    cheapest in s is C*(U, s); [(partial, ratio)]"""
    paths, env, pl, out = {}, {}, {}, []
    for e in evs:
        if e['ev'] == 'path':
            paths[e['id']] = e
        elif e['ev'] == 'env':
            env[e['rels']] = e
        elif e['ev'] == 'keep' and e['pass'] == 'main' and e['rels'] in env and env[e['rels']]['level'] == 1:
            c = vec(env[e['rels']]['partial' if e['partial'] else 'cost'], N)
            kept = [paths[i] for i in e['kept'] if paths[i]['param'] is None]
            if not kept:
                continue
            for s in range(N):
                if not math.isnan(c[s]) and c[s] > 0:
                    out.append((e['partial'], min(vec(p['total'], N)[s] for p in kept) / c[s]))
    return out


def dist(rs):
    if not rs:
        return 'none'
    n = len(rs)
    eq = sum(1 for r in rs if abs(r - 1) <= 1e-6)
    return ('%d (U, s): == C* %.1f%%, > 1.2 C* %.1f%%, > 2 C* %.1f%%, < C* %.1f%% (min %.4f), max %.2f, '
            'geo-mean %.4f' % (n, 100.0 * eq / n, 100.0 * sum(1 for r in rs if r > 1.2) / n,
                               100.0 * sum(1 for r in rs if r > 2) / n,
                               100.0 * sum(1 for r in rs if r < 1 - 1e-6) / n, min(rs), max(rs),
                               gm([max(r, 1e-300) for r in rs])))


# ------------------------------------------------------------------ main

def g5(install):
    """(ok, detail)"""
    common.start(install)
    msgs = []
    _, err = common.psql('SET main_objective_id = 9;', install, on_error_stop=False)
    ok = 'not implemented' in err
    msgs.append('objective 9 %s' % ('rejected' if ok else 'ACCEPTED: ' + err.strip()[-200:]))
    q = dict(common.load_queries())['q05_chain4']
    sets = common.parse_sets(s8_sets('lognormal', envelope=False, debug=False))
    _, err = common.psql(sets + '\nEXPLAIN %s;\n' % q, install, on_error_stop=False)
    e1 = 'robust_envelope' in err and 'ERROR' in err
    msgs.append('objective 0 without envelope %s' % ('an error' if e1 else 'NOT AN ERROR: ' + err.strip()[-200:]))
    return ok and e1, msgs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    ap.add_argument('--light', action='store_true', help='leave q12_clique8 out of the lognormal runs')
    ap.add_argument('--skip-s7', action='store_true')
    ap.add_argument('--time-only', action='store_true', help='G8 only')
    a = ap.parse_args()
    make_profile_dirs()
    make_queries()
    if a.time_only:
        g8(a.install)
        return
    allok = True
    common.stop()

    # G1
    off = runs(a.install, ['enable_rows_dist=off'], 'off')
    bad, nplan, nkeep, notall = [], 0, 0, []
    final1 = []
    for prof in ('trivial', 'ones'):
        ident = s8_sets(prof, main=3, rlimit=0, limit=BIG, final=BIG, pairing='native', dominance=True)
        for (t, ref), (_, res) in zip(off, runs(a.install, ident, 'ident_' + prof)):
            bad += [(prof, t) + b for b in same_as_stock(ref, res)]
            nplan += len(ref['queries'])
            for q, evs in all_events(res).items():
                for e in evs:
                    if e['ev'] == 'keep':
                        nkeep += 1
                        if e['kept'] != e['cands'] and sorted(e['kept']) != sorted(e['cands']):
                            notall.append('%s %s %s' % (t, q, e['rels']))
        if prof == 'trivial':
            one = s8_sets(prof, main=3, rlimit=0, limit=BIG, final=1, pairing='native', dominance=True,
                          debug=False)
            for (t, ref), (_, res) in zip(off, runs(a.install, one, 'ident_final1')):
                final1 += ['%s %s' % (t, b[0]) for b in same_as_stock(ref, res)]
    allok &= report('G1', not bad and not notall and nkeep > 0,
                    '%d plannings identical to stock, %d selections keeping all their candidates'
                    % (nplan, nkeep) if not bad and not notall else '%s %s' % (bad[:5], notall[:3]))
    print('       final relation keeping 1 path (v20): plans differing from stock: %s' % (', '.join(final1) or 'none'))

    # G2..G4
    configs = [
        ('A', dict(main=0, retain=0, limit=1, rlimit=1, final=1, tau=0.2, pairing='retained'), a.light),
        ('B', dict(main=1, retain=3, limit=2, rlimit=2, final=1, tau=0.5, pairing='retained'), True),
        ('C', dict(main=2, retain=5, limit=3, rlimit=1, final=2, tau=0.2, pairing='retained'), True),
        ('D', dict(main=4, retain=1, limit=1, rlimit=2, final=1, tau=0.2, pairing='retained'), True),
        ('E', dict(main=0, retain=0, limit=1, rlimit=1, final=1, tau=0.2, pairing='native'), a.light),
    ]
    results = {}
    total = Stats()
    per = {}
    for tag, cfg, light in configs:
        st = Stats()
        res = runs(a.install, s8_sets('lognormal', **cfg), 'logn_' + tag, light=light)
        results[tag] = res
        envs = 0
        for t, r in res:
            for q, evs in all_events(r).items():
                if 'error' in r['queries'][q]:
                    st.problems.append(('G2', '%s %s: %s' % (t, q, r['queries'][q]['error'][-200:])))
                    continue
                check_query('%s %s' % (t, q), evs, cfg, st)
                envs += sum(1 for e in evs if e['ev'] == 'env')
        need = needs_env(cfg['main']) or needs_env(cfg['retain'])
        if not need and envs:
            st.problems.append(('G5', '%d env events without an envelope objective' % envs))
        if need and not envs:
            st.problems.append(('G2', 'no env events'))
        per[tag] = (st, envs)
        for k in vars(total):
            if k == 'problems':
                total.problems += [(g, '%s: %s' % (tag, p)) for g, p in st.problems]
            else:
                setattr(total, k, getattr(total, k) + getattr(st, k))
    probs = lambda g: [p for x, p in total.problems if x == g]
    g2 = probs('G2')
    allok &= report('G2', not g2 and total.keeps > 0,
                    '%d selections of %d candidates recomputed exactly (scores, kept, why), 5 settings; '
                    '%d paths kept' % (total.keeps, total.cands, total.kept)
                    if not g2 else '%d failures: %s' % (len(g2), '; '.join(g2[:4])))
    for tag, cfg, _ in configs:
        st, envs = per[tag]
        print('       %s main %d retain %d limit %d+%d final %d tau %g %s: %d selections, %d candidates, '
              '%d kept (%d parameterized), %d level-1, %d env events, %d without envelope'
              % (tag, cfg['main'], cfg['retain'], cfg['limit'], cfg['rlimit'], cfg['final'], cfg['tau'],
                 cfg['pairing'], st.keeps, st.cands, st.kept, st.param_kept, st.level1, envs, st.noenv))
    g3 = probs('G3')
    allok &= report('G3', not g3 and total.pathlists > 0 and total.unparam > 0,
                    '%d pathlists = the kept paths in order, cheapest total the first unparameterized; '
                    '%d complete selections kept the best unparameterized path for want of one (P2)'
                    % (total.pathlists, total.unparam) if not g3 else '%d failures: %s' % (len(g3), '; '.join(g3[:4])))
    g4 = probs('G4')
    child_ok = total.child > 0 and total.child_envs > 0 and per['A'][0].child_env == per['A'][0].child
    allok &= report('G4', not g4 and total.gathers > 0 and child_ok,
                    '%d Gather candidates, all on kept partial paths (N8); %d child join rel selections, '
                    '%d against their envelope, %d child env events (N9)'
                    % (total.gathers, total.child, total.child_env, total.child_envs)
                    if not g4 and child_ok else '%s; child %d/%d env %d'
                    % ('; '.join(g4[:4]), total.child, total.child_env, total.child_envs))

    # G5
    ok, msgs = g5(a.install)
    stC = per['C']
    ok &= not probs('G5')
    allok &= report('G5', ok and stC[1] == 0 and total.level1 > 0,
                    '; '.join(msgs) + '; objectives 2/5: no env event; %d level-1 selections keep all' % total.level1)

    # G6
    print('G6         retained vs native pairing (objective 0, lognormal):')
    fa, fe = {}, {}
    for (t, ra), (_, re_) in zip(results['A'], results['E']):
        ea, ee = all_events(ra), all_events(re_)
        for q in ea:
            ka, ke = final_keep(ea[q]), final_keep(ee.get(q, []))
            if ka is None or ke is None:
                continue
            pa = {e['id']: e for e in ea[q] if e['ev'] == 'path'}
            pe = {e['id']: e for e in ee[q] if e['ev'] == 'path'}
            ia, ie = ka['cands'].index(ka['kept'][0]), ke['cands'].index(ke['kept'][0])
            fa[(t, q)] = (ka['main'][ia], mean(vec(pa[ka['kept'][0]]['total'], N)))
            fe[(t, q)] = (ke['main'][ie], mean(vec(pe[ke['kept'][0]]['total'], N)))
    for i, what in ((0, 'worst penalty'), (1, 'expected total')):
        better = sum(1 for k in fa if fa[k][i] < fe[k][i] * (1 - 1e-9))
        worse = sum(1 for k in fa if fa[k][i] > fe[k][i] * (1 + 1e-9))
        r = [fa[k][i] / fe[k][i] for k in fa if fe[k][i] > 0 and fa[k][i] > 0]
        print('       final %s: %d plannings, retained lower in %d, higher in %d, equal in %d; '
              'ratio geo-mean %.4f (min %.4f, max %.4f)'
              % (what, len(fa), better, worse, len(fa) - better - worse, gm(r) if r else 1,
                 min(r) if r else 1, max(r) if r else 1))
    worst = sorted(((fa[k][0] - fe[k][0], k) for k in fa), reverse=True)[:3]
    print('       largest worst-penalty increases with retained: %s'
          % ', '.join('%s %s %+.1f' % (k[0], k[1], d) for d, k in worst if d > 0))
    base = []
    for tag in ('A', 'E'):
        rs = []
        for t, r in results[tag]:
            for q, evs in all_events(r).items():
                rs += ratios(evs)
                if tag == 'A':
                    base += base_ratios(evs)
        for partial in (False, True):
            j = [x[2] for x in rs if x[0] > 1 and x[1] == partial]
            print('       %s join kept/C*%s: %s' % ('retained' if tag == 'A' else 'native  ',
                                                   '_p' if partial else '  ', dist(j)))
    bbad = [r for _, r in base if abs(r - 1) > 1e-9]
    allok &= report('G6', not bbad and len(base) > 0,
                    'base relations: kept == C* in all %d (U, s), complete and partial' % len(base)
                    if not bbad else '%d base (U, s) off C*: %s' % (len(bbad), bbad[:4]))

    # G7
    if not a.skip_s7:
        common.stop()
        r = subprocess.run([sys.executable, os.path.join(common.TOOLS, 'check_s7.py'), '--install', a.install],
                           capture_output=True, text=True)
        allok &= report('G7', r.returncode == 0, 'check_s7: ' + ' | '.join(
            l.split()[0] + ' ' + l.split()[1] for l in r.stdout.splitlines() if l[:1] == 'K'))

    g8(a.install)
    sys.exit(0 if allok else 1)


def g8(install):
    common.stop()
    tm = {}
    for tag, sets in (('stock', ['enable_rows_dist=off']),
                      ('s7', robust_sets(os.path.join(PROFILES, 'lognormal'), search=True, debug=False,
                                         more=['robust_select=off'])),
                      ('s8', s8_sets('lognormal', debug=False)),
                      ('s8_native', s8_sets('lognormal', pairing='native', debug=False)),
                      ('s8_noenv', s8_sets('lognormal', main=3, retain=2, debug=False)),
                      ('s8_triv', s8_sets('trivial', debug=False))):
        res = dump(os.path.join(RESULTS, 's8_time_%s.json' % tag), install, sets, repeat=5)
        tm[tag] = {q: v['planning_ms'] for q, v in res['queries'].items() if v.get('planning_ms')}
    msg = []
    for tag in ('s7', 's8', 's8_native', 's8_noenv', 's8_triv'):
        r = {q: tm[tag][q] / tm['stock'][q] for q in tm['stock'] if q in tm[tag]}
        w = max(r, key=r.get)
        msg.append('%s %.2f (max %.1f %s)' % (tag, gm(r.values()), r[w], w))
    report('G8', True, 'planning time / stock, geometric mean: ' + '; '.join(msg))
    common.stop()


if __name__ == '__main__':
    main()
