#!/usr/bin/env python3
"""
S7 gate: the join search of RobDP (optimizer/robustsearch.h): envelope pass
(one round of PostgreSQL's join search per scenario) and main pass (add_path
keeps the paths no other path dominates in every scenario).

    check_s7.py [--install v2-release] [--skip-s6]

  K1  trivial and all-ones profiles (robust_search on, dump on), benchmark,
      extra and forced-parallel queries: plans identical to stock PostgreSQL
      (robust off); lognormal: plans with the dump on identical to dump off
  K2  trivial and all-ones profiles: C*(U, s) is stock PostgreSQL's cheapest
      total cost of U, bit for bit, for every relation U and scenario s; no
      envelope path needed a replay
  K3  the envelope pass leaves nothing behind: lognormal, the main pass's
      rel, ppi, path and pathlist events are identical with robust_envelope
      on and off
  K4  envelope against PostgreSQL: a one-point profile that scales a filter
      by K gives, for every relation U whose rows (and whose parts' rows)
      agree, C*(U, 0) = stock PostgreSQL's cheapest total cost of U for the
      query whose filter selects K times as much (relative 1e-6; 1% for the
      partitioned cases of check_s6 C4)
  K5  envelope as a lower bound, lognormal: for every relation U and
      scenario s, the cheapest unparameterized non-partial path the main
      pass keeps costs at least C*(U, s) (to 1e-6) for base relations, and
      the distribution of that ratio for join relations (informational)
  K6  dominance: no path the main pass keeps is dominated in every scenario
      by another it keeps (same parameterization, the other unordered or with
      a subset of its pathkeys, beyond the fuzz factor in total and startup
      cost, no more rows, parallel safety no worse); and the dominance keeps
      paths stock PostgreSQL would have pruned
  K7  S6 gates (and through them S5..S1) still pass (robust_search off)
  K8  planning time, robust_search on vs off and vs stock (informational)
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys

import common
from check_s4 import (PROFILES, EXTRA, RESULTS, all_events, dump, make_profile_dirs, robust_sets,
                      same_as_stock, vec, report)
from check_s6 import C4_CASES, PARALLEL, close
from robdp_dump import parse

N = 20
FUZZ = 1.01
# S7's search: add_path keeps the paths no other dominates in every scenario,
# without S8's selection (robust_select off)
SEARCH = lambda prof, **kw: robust_sets(os.path.join(PROFILES, prof), search=True, more=['robust_select=off'], **kw)


def index(evs):
    """paths by id; pathlist, env and rel events by rels"""
    paths, pl, env, rel = {}, {}, {}, {}
    stat = None
    for e in evs:
        if e['ev'] == 'path':
            paths[e['id']] = e
        elif e['ev'] == 'pathlist':
            pl[e['rels']] = e
        elif e['ev'] == 'env':
            env[e['rels']] = e
        elif e['ev'] == 'rel' and e.get('first', True):
            rel[e['rels']] = e
        elif e['ev'] == 'envstat':
            stat = e
    return paths, pl, env, rel, stat


# Lognormal dumps of every path of q12_clique8 take 8 GB (S7 keeps tens of
# thousands of paths there): the debug runs with a varying profile leave it out
HEAVY = ('q12_clique8',)
LIGHT = os.path.join(common.ROOT, 'data', 's7_queries')


def make_light():
    os.makedirs(LIGHT, exist_ok=True)
    src = os.path.join(common.TOOLS, 'queries')
    for f in os.listdir(LIGHT):
        os.remove(os.path.join(LIGHT, f))
    for f in os.listdir(src):
        if f.endswith('.sql') and f[:-4] not in HEAVY:
            shutil.copy(os.path.join(src, f), LIGHT)


def runs(install, sets, tag, light=False):
    """benchmark, extra and forced-parallel dumps with these settings"""
    out = []
    bench = LIGHT if light else None
    for qdir, extra, t in ((bench, [], 'bench'), (EXTRA, [], 'extra'), (bench, PARALLEL, 'par')):
        res = dump(os.path.join(RESULTS, 's7_%s_%s.json' % (tag, t)), install, extra + sets, qdir)
        out.append((t, extra, qdir, res))
    return out


# ------------------------------------------------------------------ K2

def envelope_is_stock(evs, n):
    """(relations checked, problems): C*(U, s) == cheapest total, bit for bit"""
    paths, pl, env, _, stat = index(evs)
    bad, k = [], 0
    for rels, e in env.items():
        p = paths.get(pl.get(rels, {}).get('cheapest_total'))
        if p is None:
            bad.append('%s: no cheapest path' % rels)
            continue
        k += 1
        c = vec(e['cost'], n)
        if any(x != p['total_point'] for x in c):
            bad.append('%s: envelope %s, cheapest %r' % (rels, c[:3], p['total_point']))
    if stat is not None and (stat['replay'] or stat['mismatch'] or stat['unsupported'] or stat['nocontext']):
        bad.append('envelope paths: %s' % {k: stat[k] for k in ('replay', 'mismatch', 'unsupported', 'nocontext')})
    return k, bad


# ------------------------------------------------------------------ K3

def main_events(evs):
    return [json.dumps(e, sort_keys=True) for e in evs if e['ev'] in ('rel', 'ppi', 'path', 'pathlist')]


# ------------------------------------------------------------------ K4

def planning(sql, pdir, install, n=1):
    q = ("SET enable_rows_dist=on; SET robust_ec_forest=on; SET robust_search=on; SET robust_envelope=on; SET robust_select=off;\n"
         "SET error_profile_path='%s'; SET error_sample_count=%d; SET error_sample_kde_bandwidth=0;\n"
         "SET robust_debug_dump=on; SET client_min_messages=log;\n"
         "EXPLAIN %s;\n" % (pdir, n, sql))
    _, err = common.psql(q, install, on_error_stop=False)
    evs = []
    for l in err.splitlines():
        if 'ROBDP ' in l:
            for k, v in parse([l]).items():
                for e in v:
                    e['ev'] = k
                    evs.append(e)
    return evs


def names(rels):
    return frozenset(rels.split())


def k4(install):
    """returns (cases, relations compared, relations whose envelope differs
    from the point, relations not compared, approximate cases, problems)"""
    trivial = os.path.join(PROFILES, 'trivial')
    cases, compared, moved, skipped, approx, problems = 0, 0, 0, 0, [], []
    for K in (0.5, 2.0, 3.0):
        pdir = os.path.join(PROFILES, 's6_scale_%g' % K)
        os.makedirs(pdir, exist_ok=True)
        for alias in ('t1', 't2', 'p'):
            with open(os.path.join(pdir, alias + '.txt'), 'w') as f:
                f.write('%r 0.2\n' % (0.2 * K))
        for name, tmpl, alias, thr in C4_CASES:
            scn = planning(tmpl.format(thr=thr), pdir, install)
            stk = planning(tmpl.format(thr=int(round(thr * K))), trivial, install)
            pt = planning(tmpl.format(thr=thr), trivial, install)
            cases += 1
            _, _, env, rel1, _ = index(scn)
            p2, pl2, _, rel2, _ = index(stk)
            _, _, envp, _, _ = index(pt)
            if not env:
                continue        # a single relation: no join search
            b1 = {r: vec(e['rows'], 1)[0] for r, e in rel1.items() if e['kind'] in ('base', 'other')}
            b2 = {r: e['rows_point'] for r, e in rel2.items() if e['kind'] in ('base', 'other')}
            if b1.keys() != b2.keys():
                problems.append('%s K=%g: different base rels' % (name, K))
                continue
            d = max(abs(b1[r] - b2[r]) / b2[r] for r in b1)
            if d > 1e-3:
                problems.append('%s K=%g: base rows differ by %.2g' % (name, K, d))
                continue
            tol = 1e-6 if d == 0 else 1e-2
            if d:
                approx.append('%s K=%g' % (name, K))
            # relations whose rows differ from stock (the S5 model holds
            # some join selectivities at the point), and all above them
            off = [names(r) for r in rel1 if r in rel2 and
                   not close(vec(rel1[r]['rows'], 1)[0], rel2[r]['rows_point'], tol)]
            for rels, e in env.items():
                if any(o <= names(rels) for o in off):
                    skipped += 1
                    continue
                p = p2.get(pl2.get(rels, {}).get('cheapest_total'))
                if p is None:
                    problems.append('%s K=%g %s: not in the stock planning' % (name, K, rels))
                    continue
                got = vec(e['cost'], 1)[0]
                compared += 1
                if rels in envp and vec(envp[rels]['cost'], 1)[0] != got:
                    moved += 1
                if not close(got, p['total_point'], tol):
                    problems.append('%s K=%g %s: envelope %r, stock %r' % (name, K, rels, got, p['total_point']))
    return cases, compared, moved, skipped, approx, problems


# ------------------------------------------------------------------ K5

def lower_bound(evs, n):
    """[(level, ratio)] of the cheapest kept path over C*(U, s), per (U, s)"""
    paths, pl, env, _, _ = index(evs)
    out = []
    for rels, e in env.items():
        kept = [paths[i] for i in pl.get(rels, {}).get('paths', []) if i in paths]
        kept = [p for p in kept if p['param'] is None and not p['partial']]
        if not kept:
            continue
        c = vec(e['cost'], n)
        tot = [vec(p['total'], n) for p in kept]
        for s in range(n):
            m = min(t[s] for t in tot)
            out.append((e['level'], m / c[s] if c[s] > 0 else 1.0, rels, s))
    return out


# ------------------------------------------------------------------ K6

def dominated(a, b, n):
    """does a dominate b in every scenario, clearly (add_path must have kept
    only a)?"""
    # add_path takes a parameterized path's pathkeys as empty
    if a['param'] != b['param'] or a['partial'] != b['partial'] or \
            (b['param'] is None and b['npathkeys'] != 0):
        return False
    if a.get('parallel_safe', True) < b.get('parallel_safe', True):
        return False
    ta, tb = vec(a['total'], n), vec(b['total'], n)
    if not all(x * FUZZ < y for x, y in zip(ta, tb)):
        return False
    if not a['partial']:
        sa, sb = vec(a['startup'], n), vec(b['startup'], n)
        if not all(x * FUZZ < y or (x == 0 and y == 0) for x, y in zip(sa, sb)):
            return False
        ra, rb = vec(a['rows'], n), vec(b['rows'], n)
        if not all(x <= y for x, y in zip(ra, rb)):
            return False
    return True


def dominance(evs, n):
    """(pairs checked, problems, kept paths)"""
    paths, pl, _, _, _ = index(evs)
    pairs, bad, kept = 0, [], 0
    for rels, e in pl.items():
        for lst in ('paths', 'partial'):
            ps = [paths[i] for i in e[lst] if i in paths]
            kept += len(ps)
            for a in ps:
                for b in ps:
                    if a is b:
                        continue
                    pairs += 1
                    if dominated(a, b, n):
                        bad.append('%s: %s %d dominates %s %d' % (rels, a['type'], a['id'], b['type'], b['id']))
    return pairs, bad, kept


# ------------------------------------------------------------------ main

def gm(r):
    return math.exp(sum(math.log(x) for x in r) / len(r))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    ap.add_argument('--skip-s6', action='store_true')
    a = ap.parse_args()
    make_profile_dirs()
    stock = json.load(open(os.path.join(RESULTS, 'stock_v2rel.json')))
    allok = True
    common.stop()

    # K1, K2
    off_runs = runs(a.install, ['enable_rows_dist=off'], 'off')
    bad_plans, nplan, k2_rels, k2_bad = [], 0, 0, []
    for prof in ('trivial', 'ones'):
        for (t, extra, qdir, res), (_, _, _, off) in zip(runs(a.install, SEARCH(prof, bins=3), prof), off_runs):
            ref = stock if t == 'bench' else off
            bad_plans += [(prof, t) + b for b in same_as_stock(ref, res)]
            nplan += len(ref['queries'])
            for name, evs in all_events(res).items():
                k, bad = envelope_is_stock(evs, N)
                k2_rels += k
                k2_bad += ['%s %s %s: %s' % (prof, t, name, b) for b in bad]
    make_light()
    logn_dbg = runs(a.install, SEARCH('lognormal'), 'logn', light=True)
    logn_nodbg = runs(a.install, SEARCH('lognormal', debug=False), 'logn_nodump')
    for (t, _, _, r1), (_, _, _, r2) in zip(logn_dbg, logn_nodbg):
        r2 = {'queries': {q: v for q, v in r2['queries'].items() if q in r1['queries']}}
        bad_plans += [('lognormal dump', t) + b for b in same_as_stock(r2, r1)]
        nplan += len(r2['queries'])
    allok &= report('K1', not bad_plans, '%d plannings identical (trivial/ones vs stock, lognormal dump on vs off)'
                    % nplan if not bad_plans else str(bad_plans[:5]))
    allok &= report('K2', not k2_bad and k2_rels > 0,
                    'C*(U, s) == stock cheapest total for %d relations x %d scenarios, no envelope replay'
                    % (k2_rels, N) if not k2_bad else '%d failures: %s' % (len(k2_bad), '; '.join(k2_bad[:4])))

    # K3
    k3_bad, k3_n = [], 0
    noenv = runs(a.install, ['robust_envelope=off'] + SEARCH('lognormal'), 'logn_noenv', light=True)
    for (t, _, _, r1), (_, _, _, r2) in zip(logn_dbg, noenv):
        e1, e2 = all_events(r1), all_events(r2)
        for name in e1:
            m1, m2 = main_events(e1[name]), main_events(e2.get(name, []))
            k3_n += len(m1)
            if m1 != m2:
                i = next((j for j, (x, y) in enumerate(zip(m1, m2)) if x != y), min(len(m1), len(m2)))
                k3_bad.append('%s %s: event %d of %d/%d differs' % (t, name, i, len(m1), len(m2)))
    allok &= report('K3', not k3_bad and k3_n > 0,
                    '%d main-pass events identical with the envelope pass on and off' % k3_n
                    if not k3_bad else '; '.join(k3_bad[:4]))

    # K4
    common.start(a.install)
    cases, compared, moved, skipped, approx, problems = k4(a.install)
    allok &= report('K4', not problems and compared > 0 and moved > 0,
                    '%d cases, %d relations: C*(U, 0) equal to stock PostgreSQL\'s cheapest total of the scaled '
                    'query (%d differing from the unscaled one; %d partitioned cases to 1%%); %d relations not '
                    'compared (their rows or their parts\' differ from stock)'
                    % (cases, compared, moved, len(approx), skipped)
                    if not problems else '%d failures: %s' % (len(problems), '; '.join(problems[:4])))

    # K5, K6
    ratios, pairs, dom_bad, kept, kept_stock = [], 0, [], 0, 0
    triv = {t: res for t, _, _, res in runs(a.install, SEARCH('trivial'), 'triv_k6', light=True)}
    for t, _, _, res in logn_dbg:
        for name, evs in all_events(res).items():
            ratios += lower_bound(evs, N)
            p, b, k = dominance(evs, N)
            pairs += p
            dom_bad += ['%s %s: %s' % (t, name, x) for x in b]
            kept += k
        for name, evs in all_events(triv[t]).items():
            kept_stock += sum(len(e['paths']) + len(e['partial']) for e in evs if e['ev'] == 'pathlist')
    base = [r for lv, r, _, _ in ratios if lv == 1]
    join = [r for lv, r, _, _ in ratios if lv > 1]
    base_bad = [x for x in ratios if x[0] == 1 and x[1] < 1 - 1e-6]
    below = [x for x in ratios if x[0] > 1 and x[1] < 1 - 1e-6]
    above = [x for x in ratios if x[0] > 1 and x[1] > 1 + 1e-6]
    allok &= report('K5', not base_bad and len(join) > 0,
                    '%d (base rel, s): kept >= C*; %d (join rel, s): kept == C* in %.1f%%, below in %.1f%% '
                    '(min %.4f), above in %.1f%% (max %.3f)'
                    % (len(base), len(join), 100.0 * (len(join) - len(below) - len(above)) / len(join),
                       100.0 * len(below) / len(join), min(join), 100.0 * len(above) / len(join), max(join))
                    if not base_bad else '%d base relations below C*: %s' % (len(base_bad), base_bad[:3]))
    if join:
        print('       join (U, s) with kept/C* above 1.01: %.1f%%, 1.1: %.1f%%, 2: %.1f%%; worst: %s'
              % tuple([100.0 * sum(1 for r in join if r > b) / len(join) for b in (1.01, 1.1, 2)] +
                      [', '.join('%s s=%d %.2f' % (r, s_, x) for _, x, r, s_ in
                                 sorted(above, key=lambda x: -x[1])[:3])]))
    if below:
        w = sorted(below, key=lambda x: x[1])[:3]
        print('       lowest: ' + ', '.join('%s s=%d %.4f' % (r, s, x) for _, x, r, s in w))
    allok &= report('K6', not dom_bad and kept > kept_stock,
                    '%d pairs of kept paths, none dominated; %d paths kept (lognormal) vs %d (trivial = stock)'
                    % (pairs, kept, kept_stock) if not dom_bad else '%d dominated: %s' % (len(dom_bad), dom_bad[:4]))

    # K7
    if not a.skip_s6:
        common.stop()
        r = subprocess.run([sys.executable, os.path.join(common.TOOLS, 'check_s6.py'), '--install', a.install],
                           capture_output=True, text=True)
        allok &= report('K7', r.returncode == 0, 'check_s6: ' + ' | '.join(
            l.split()[0] + ' ' + l.split()[1] for l in r.stdout.splitlines() if l[:1] == 'C'))

    # K8
    common.stop()
    t = {}
    for tag, sets in (('stock', ['enable_rows_dist=off']),
                      ('s6', robust_sets(os.path.join(PROFILES, 'trivial'), debug=False)),
                      ('triv', SEARCH('trivial', debug=False)),
                      ('logn', SEARCH('lognormal', debug=False)),
                      ('logn_noenv', ['robust_envelope=off'] + SEARCH('lognormal', debug=False))):
        res = dump(os.path.join(RESULTS, 's7_time_%s.json' % tag), a.install, sets, repeat=5)
        t[tag] = {q: v['planning_ms'] for q, v in res['queries'].items() if v.get('planning_ms')}
    msg = []
    for tag in ('s6', 'triv', 'logn', 'logn_noenv'):
        r = {q: t[tag][q] / t['stock'][q] for q in t['stock'] if q in t[tag]}
        w = max(r, key=r.get)
        msg.append('%s %.2f (max %.1f %s)' % (tag, gm(r.values()), r[w], w))
    report('K8', True, 'planning time / stock, geometric mean: ' + '; '.join(msg))
    common.stop()
    sys.exit(0 if allok else 1)


if __name__ == '__main__':
    main()
