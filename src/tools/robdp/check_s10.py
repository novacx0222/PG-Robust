#!/usr/bin/env python3
"""
S10 gate: the paper's objectives and diversification functions
(optimizer/robustselect.h, objectives.py), v20's numbering.

    check_s10.py [--install v2-release] [--light] [--skip-s9] [--time-only]

  O1  settings: the main objective takes the objectives only (a
      diversification function, 9, 19 are rejected), the retain strategy
      objectives and diversification functions; robust_penalty_lambda and
      robust_quantile_alpha reject values out of range; an objective or
      strategy that compares to the envelope is an error with
      robust_envelope off, the others (2-5, 8, 17, 18; Rand, E[Cost]-Join)
      run without it
  O2  lognormal, 11 settings covering every objective as the main one and
      every objective and diversification function as the retain strategy:
      every selection of the main pass (scores, the paths kept and why, the
      order a diversification function picks in), every scoring of the
      scan/join relation and every final pick recomputed exactly; settings
      that need no envelope run no envelope round
  O3  the gates see a difference: the same dumps recomputed with lambda,
      alpha or tau changed (the scores they enter) fail
  O4  v20 parity: objective 12 with alpha 0.85 (the default) at N = 20 is
      v20's "4th worst penalty"; objective 0 is v20's worst penalty
  O5  Rand is a function of the query: the two plannings of each query in
      one session keep the same paths with the same draws (v20's generator
      was the backend's, so the second differed), two runs agree, and two
      relations do not draw the same sequence
  O6  informational: per setting, the plans that differ from objective 0
      without retained paths, and the final plan's cost against the best
      final candidate of any setting in each scenario (the planner's cost
      model, not execution)
  O7  check_s9 (through it S8 .. S1) still passes
  O8  planning time / stock per setting (informational)
"""
import argparse
import json
import math
import os
import subprocess
import sys

import common
import compare
import objectives
from check_s4 import PROFILES, RESULTS, _events, all_events, dump, make_profile_dirs, report, vec
from check_s7 import gm
from check_s8 import LIGHT, N, Stats, check_query, make_queries, s8_sets
from check_s9 import S9Q, check_planning, fractional, make_s9_queries, new_stats, runs

# tag, settings (limit 1, final 1, tau 0.2 unless given)
CONFIGS = [
    ('O6', dict(main=6, retain=12, rlimit=2)),
    ('O10', dict(main=10, retain=11, rlimit=2, lam=0.5)),
    ('O12', dict(main=12, retain=17, rlimit=2, alpha=0.95)),
    ('O8', dict(main=8, retain=18, rlimit=2)),
    ('O17', dict(main=17, retain=13, rlimit=2)),
    ('O18', dict(main=18, retain=16, rlimit=3)),
    ('R7', dict(main=0, retain=7, rlimit=2)),
    ('R13', dict(main=0, retain=13, rlimit=3)),
    ('R14', dict(main=0, retain=14, rlimit=7)),
    ('R15', dict(main=11, retain=15, rlimit=3, lam=2.0)),
    ('R16', dict(main=3, retain=16, rlimit=3)),
]
# informational baselines
BASES = [
    ('B0', dict(main=0, retain=0, rlimit=0)),
    ('B3', dict(main=3, retain=0, rlimit=0)),
    ('B8', dict(main=8, retain=0, rlimit=0)),
]


def full(cfg):
    c = dict(main=0, retain=0, limit=1, rlimit=1, final=1, tau=0.2)
    c.update(cfg)
    return c


def plannings(messages):
    """the events of each planning, split at the "query" events"""
    out, cur = [], None
    for l in messages:
        if '"ev":"query"' in l:
            cur = []
            out.append(cur)
        if cur is not None:
            cur.append(l)
    return [list(_events(p)) for p in out]


# ------------------------------------------------------------------ O1

def o1(install):
    common.start(install)
    probs, n = [], 0

    def try_set(sql):
        _, err = common.psql(sql, install, on_error_stop=False)
        return 'ERROR' not in err, err

    for v in list(range(-1, 20)):
        for guc, allowed in (('main_objective_id', objectives.OBJECTIVES),
                             ('retain_strategy_id', objectives.OBJECTIVES + objectives.DIVERSIFIERS)):
            ok, err = try_set('SET %s = %d;' % (guc, v))
            n += 1
            if ok != (v in allowed):
                probs.append('%s = %d %s' % (guc, v, 'accepted' if ok else 'rejected: ' + err.strip()[-120:]))
    for guc, good, bad_ in (('robust_penalty_lambda', (0, 0.5, 1000), (-0.1, 1001)),
                            ('robust_quantile_alpha', (0.01, 0.5, 1), (0, 1.01))):
        for v in good + bad_:
            ok, _ = try_set('SET %s = %r;' % (guc, v))
            n += 1
            if ok != (v in good):
                probs.append('%s = %r %s' % (guc, v, 'accepted' if ok else 'rejected'))
    q = dict(common.load_queries())['q05_chain4']
    for main, retain, rl in ((6, 0, 0), (12, 0, 0), (3, 13, 2), (3, 14, 2), (3, 15, 2), (2, 6, 2),
                             (3, 16, 2), (8, 7, 2), (17, 18, 2), (5, 16, 2)):
        need = objectives.needs_env(main) or (rl > 0 and objectives.needs_env(retain))
        sets = common.parse_sets(s8_sets('lognormal', main=main, retain=retain, rlimit=rl, envelope=False,
                                         debug=False))
        _, err = common.psql(sets + '\nEXPLAIN %s;\n' % q, install, on_error_stop=False)
        n += 1
        erred = 'ERROR' in err and 'robust_envelope' in err
        if erred != need or ('ERROR' in err and not need):
            probs.append('main %d retain %d without envelope: %s' % (main, retain, err.strip()[-160:] or 'ran'))
    return probs, n


# ------------------------------------------------------------------ O2

def check_all(res, cfg, st8, st9, on_final=None, tag=''):
    envs = 0
    for t, r in res:
        for q, evs in all_events(r).items():
            if 'error' in r['queries'][q]:
                st8.problems.append(('O2', '%s %s: %s' % (t, q, r['queries'][q]['error'][-200:])))
                continue
            where = '%s %s %s' % (tag, t, q)
            check_query(where, evs, cfg, st8)
            check_planning(where, evs, cfg, st9,
                           (lambda e, paths, t=t, q=q: on_final(t, q, e, paths)) if on_final else None)
            envs += sum(1 for e in evs if e['ev'] == 'env')
    return envs


# ------------------------------------------------------------------ O4

def v20_fourth_worst(p, env, tau):
    """v20 path_strategy 12: the 4th largest Penalty(s)"""
    t = vec(p['total'], N)
    pen = sorted((t[s] - env[s] if env is not None and t[s] > env[s] * (1.0 + tau) else 0.0
                  for s in range(N)), reverse=True)
    return pen[3]


def o4(res, cfg):
    """objective 12's scores (alpha 0.85) against v20's 4th worst penalty"""
    n, bad = 0, []
    for t, r in res:
        for q, evs in all_events(r).items():
            paths, env = {}, {}
            for e in evs:
                if e['ev'] == 'path':
                    paths[e['id']] = e
                elif e['ev'] == 'env':
                    env[e['rels']] = e
                elif e['ev'] == 'keep' and e['pass'] == 'main' and e['envelope']:
                    ev = vec(env[e['rels']]['partial' if e['partial'] else 'cost'], N)
                    ev, _ = objectives.selection_envelope([paths[i] for i in e['cands']], e['partial'], ev,
                                                          True, N)
                    for i, sc in zip(e['cands'], e['main']):
                        n += 1
                        v = v20_fourth_worst(paths[i], ev, cfg['tau'])
                        if v != sc:
                            bad.append('%s %s %s: %r, v20 %r' % (t, q, e['rels'], sc, v))
    return n, bad


# ------------------------------------------------------------------ O5

def keep_sig(evs):
    return [(e['rels'], e['partial'], e['cands'], e.get('retain_score'), e['kept'], e['by'])
            for e in evs if e['ev'] == 'keep' and e['pass'] == 'main']


def o5(res1, res2):
    nq, nkeep, nseq, bad = 0, 0, 0, []
    dup = 0
    for (t, r1), (_, r2) in zip(res1, res2):
        for q in r1['queries']:
            p1 = plannings(r1['queries'][q].get('messages', []))
            p2 = plannings(r2['queries'][q].get('messages', []))
            if len(p1) < 2 or not p2:
                continue
            nq += 1
            a, b, c = keep_sig(p1[0]), keep_sig(p1[1]), keep_sig(p2[0])
            nkeep += len(a)
            if a != b:
                bad.append('%s %s: second planning in the session differs' % (t, q))
            if a != c:
                bad.append('%s %s: second run differs' % (t, q))
            seqs = set()
            for rels, partial, cands, rs, kept, by in a:
                draws = tuple(x for x in rs or [] if x not in (0, 0.0))
                if len(draws) >= 2:
                    nseq += 1
                    dup += draws in seqs
                    seqs.add(draws)
    if dup:
        bad.append('%d relations draw the sequence another relation of their planning drew' % dup)
    return nq, nkeep, nseq, bad


# ------------------------------------------------------------------ O6

class Quality:
    """the top-level final pick of each query (the last final event), its
    F(s), and every candidate's F(s), per setting"""

    def __init__(self):
        self.last = {}          # tag -> {(kind, q): (chosen F, min F of the candidates)}

    def hook(self, tag):
        def f(t, q, e, paths):
            F = {i: [fractional(paths[i], e['fraction'], s) for s in range(N)] for i in e['cands']}
            ch = e['chosen'] if e['chosen'] != -1 else e['pg']
            self.last.setdefault(tag, {})[(t, q)] = \
                (F.get(ch), [min(F[i][s] for i in F) for s in range(N)]) if ch in F else None
        return f

    def table(self, tags):
        self.chosen = {tag: {k: v[0] for k, v in self.last.get(tag, {}).items() if v} for tag in tags}
        self.best = {}
        for tag in tags:
            for k, v in self.last.get(tag, {}).items():
                if v:
                    b = self.best.get(k)
                    self.best[k] = v[1] if b is None else [min(x, y) for x, y in zip(b, v[1])]
        rows = []
        keys = None
        for tag in tags:
            ks = set(self.chosen.get(tag, {}))
            keys = ks if keys is None else keys & ks
        keys = sorted(k for k in (keys or ()) if k[0] == 'bench')
        for tag in tags:
            mr, wr, pp, ef, mp = [], [], 0.0, [], []
            for k in keys:
                f, b = self.chosen[tag][k], self.best[k]
                r = [x / y if y > 0 else 1.0 for x, y in zip(f, b)]
                mr.append(sum(r) / N)
                wr.append(max(r))
                pp += sum(1 for x in r if x > 1.2) / N
                ef.append(sum(f) / sum(b))
                mp.append(1.0 + max(x - y for x, y in zip(f, b)) / max(b))
            rows.append((tag, len(keys), gm(ef), gm(mp), gm(mr), gm(wr), max(wr) if wr else 0,
                         pp / max(len(keys), 1)))
        return rows


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    ap.add_argument('--light', action='store_true', help='check_s9 with --light')
    ap.add_argument('--skip-s9', action='store_true')
    ap.add_argument('--time-only', action='store_true', help='O8 only')
    a = ap.parse_args()
    make_profile_dirs()
    make_queries()
    make_s9_queries()
    if a.time_only:
        o8(a.install)
        return
    allok = True
    common.stop()

    # O1
    probs, n = o1(a.install)
    allok &= report('O1', not probs, '%d settings tried: objectives %s main only via the objective GUC, '
                    'diversification functions %s retain only; lambda, alpha ranges; envelope needed exactly by '
                    'Penalty-based ones' % (n, list(objectives.OBJECTIVES), list(objectives.DIVERSIFIERS))
                    if not probs else '; '.join(probs[:6]))

    # O2, O6
    quality = Quality()
    results, per, total8, total9 = {}, {}, Stats(), new_stats()
    for tag, cfg in CONFIGS + BASES:
        cfg = full(cfg)
        st8, st9 = Stats(), new_stats()
        res = runs(a.install, s8_sets('lognormal', **cfg), 's10_' + tag, light=True)
        results[tag] = res
        envs = check_all(res, cfg, st8, st9, quality.hook(tag), tag)
        need = objectives.needs_env(cfg['main']) or (cfg['rlimit'] > 0 and objectives.needs_env(cfg['retain']))
        if need != (envs > 0):
            st8.problems.append(('O2', '%d env events, envelope %sneeded' % (envs, '' if need else 'not ')))
        per[tag] = (cfg, st8, st9, envs)
        total8.problems += [(g, '%s: %s' % (tag, p)) for g, p in st8.problems]
        total9['problems'] += [(g, '%s: %s' % (tag, p)) for g, p in st9['problems']]
        for k in ('keeps', 'cands', 'kept', 'dv_picks'):
            setattr(total8, k, getattr(total8, k) + getattr(st8, k))
        for k in ('finals', 'picked', 'differs', 'upper_keeps'):
            total9[k] += st9[k]
    probs = [p for _, p in total8.problems] + [p for _, p in total9['problems']]
    allok &= report('O2', not probs and total8.dv_picks > 0,
                    '%d settings: %d selections of %d candidates, %d final picks (%d with vectors), %d scan/join '
                    'scorings recomputed exactly; %d paths retained by diversification functions'
                    % (len(CONFIGS) + len(BASES), total8.keeps, total8.cands, total9['finals'], total9['picked'],
                       total9['upper_keeps'], total8.dv_picks)
                    if not probs else '%d failures: %s' % (len(probs), '; '.join(probs[:5])))
    for tag, _ in CONFIGS + BASES:
        cfg, st8, st9, envs = per[tag]
        extra = ''.join(' %s %g' % (k, cfg[k]) for k in ('lam', 'alpha') if k in cfg)
        print('       %-4s main %2d %-18s retain %2d %-16s K %d+%d%s: %5d selections, %5d kept (%4d by the '
              'function), %4d final picks (%3d differ from PostgreSQL\'s), %d env events'
              % (tag, cfg['main'], objectives.NAMES[cfg['main']], cfg['retain'],
                 objectives.NAMES.get(cfg['retain'], '-') if cfg['rlimit'] else '-', cfg['limit'], cfg['rlimit'],
                 extra, st8.keeps, st8.kept, st8.dv_picks, st9['picked'], st9['differs'], envs))

    # O3
    caught = []
    for tag, change in (('O10', dict(lam=0.75)), ('O12', dict(alpha=0.9)), ('R15', dict(lam=1.0)),
                        ('O6', dict(tau=0.25)), ('R13', dict(tau=0.3))):
        cfg = dict(per[tag][0], **change)
        st8, st9 = Stats(), new_stats()
        check_all(results[tag], cfg, st8, st9)
        caught.append((tag, change, len(st8.problems) + len(st9['problems'])))
    allok &= report('O3', all(c[2] > 0 for c in caught),
                    'recomputed with a wrong setting: ' + ', '.join('%s %s: %d failures' % (t, ch, n)
                                                                     for t, ch, n in caught))

    # O4
    st = Stats()
    cfg12 = full(dict(main=12, retain=0, rlimit=0))
    res12 = runs(a.install, s8_sets('lognormal', **cfg12), 's10_v20q', light=True)
    envs = check_all(res12, cfg12, st, new_stats())
    n, bad = o4(res12, cfg12)
    allok &= report('O4', not bad and not st.problems and n > 0,
                    '%d scores of objective 12 (alpha 0.85, N 20) equal to v20\'s 4th worst penalty' % n
                    if not bad and not st.problems else '; '.join((bad + [p for _, p in st.problems])[:5]))

    # O5
    res7b = runs(a.install, s8_sets('lognormal', **full(CONFIGS[6][1])), 's10_R7b', light=True)
    nq, nkeep, nseq, bad = o5(results['R7'], res7b)
    allok &= report('O5', not bad and nseq > 0,
                    '%d queries, %d selections: the same draws and paths in the second planning of the session and '
                    'in a second run; %d distinct draw sequences' % (nq, nkeep, nseq)
                    if not bad else '; '.join(bad[:5]))

    # O6
    print('O6         informational (lognormal, serial benchmark, the planner\'s cost vectors):')
    base = results['B0']
    for tag, _ in CONFIGS + BASES:
        if tag == 'B0':
            continue
        ch = []
        for (t, r0), (_, r1) in zip(base, results[tag]):
            for q in r0['queries']:
                p0, p1 = r0['queries'][q].get('plan'), r1['queries'][q].get('plan')
                if p0 and p1 and compare.compare_plans(p0, p1, 1e-9)[0] == 'PLAN-DIFF':
                    ch.append('%s %s' % (t, q))
        print('       %-4s %3d plans differ from B0 (objective 0, nothing retained): %s'
              % (tag, len(ch), ', '.join(ch[:6]) + (' ...' if len(ch) > 6 else '')))
    print('       the final plan\'s F(s) against best(s), the cheapest final candidate of any setting in s, '
          'geometric means over the queries: absolute (E[F] / E[best], 1 + max Penalty / max best) and '
          'per scenario (mean and worst F/best; largest worst; share of scenarios above 1.2)')
    for tag, nq, ef, mp, mr, wr, mx, pp in quality.table([t for t, _ in CONFIGS + BASES]):
        print('       %-4s %3d  E[F] %.3f  maxPen %.3f | mean %.3f  worst %.3f  max %9.2f  >1.2 %.3f'
              % (tag, nq, ef, mp, mr, wr, mx, pp))

    # O7
    if not a.skip_s9:
        common.stop()
        r = subprocess.run([sys.executable, os.path.join(common.TOOLS, 'check_s9.py'), '--install', a.install]
                           + (['--light'] if a.light else []), capture_output=True, text=True)
        allok &= report('O7', r.returncode == 0, 'check_s9: ' + ' | '.join(
            l.split()[0] + ' ' + l.split()[1] for l in r.stdout.splitlines() if l[:1] == 'U'))
        if r.returncode != 0:
            print(r.stdout[-3000:])

    o8(a.install)
    sys.exit(0 if allok else 1)


def o8(install):
    common.stop()
    tm = {}
    sets = [('stock', ['enable_rows_dist=off'])] + \
        [(tag, s8_sets('lognormal', debug=False, **full(cfg))) for tag, cfg in BASES[:1] + CONFIGS]
    for tag, s in sets:
        res = dump(os.path.join(RESULTS, 's10_time_%s.json' % tag), install, s, LIGHT, repeat=3)
        tm[tag] = {q: v['planning_ms'] for q, v in res['queries'].items() if v.get('planning_ms')}
    msg = []
    for tag, _ in sets[1:]:
        r = {q: tm[tag][q] / tm['stock'][q] for q in tm['stock'] if q in tm[tag]}
        w = max(r, key=r.get)
        msg.append('%s %.2f (max %.1f %s)' % (tag, gm(r.values()), r[w], w))
    report('O8', True, 'planning time / stock, geometric mean (benchmark without q12_clique8): ' + '; '.join(msg))
    common.stop()


if __name__ == '__main__':
    main()
