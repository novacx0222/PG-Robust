#!/usr/bin/env python3
"""
S1 gate: the RobDP skeleton must not change planning, and the debug dump
must reach the harness exactly.

    check_s1.py [--install v2-release]

Checks:
  G1  robust off            -> every query plans exactly like stock
  G2  robust on + profiles  -> still exactly like stock (nothing uses them yet)
  G3  dump on               -> one "query" event per planning, with the
                               settings in effect
  G4  dump number format    -> test_robust_dump()'s values parse back to the
                               exact doubles (needs the test_robust module
                               installed in the build)
  G5  planning time, robust off vs stock (informational, noise level)
"""
import argparse
import json
import math
import os
import subprocess
import sys

import common
import compare
from robdp_dump import parse

RESULTS = os.path.join(common.TOOLS, 'results')
PROFILES = os.path.join(common.ROOT, 'data', 'profiles')


def dump(out, install, sets, repeat=5):
    subprocess.run([sys.executable, os.path.join(common.TOOLS, 'explain_dump.py'), out,
                    '--install', install, '--repeat', str(repeat)] +
                   sum([['--set', s] for s in sets], []), check=True, capture_output=True)
    return json.load(open(out))


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    a = ap.parse_args()
    stock = json.load(open(os.path.join(RESULTS, 'stock_v2rel.json')))
    n = len(stock['queries'])
    allok = True

    # G1
    off = dump(os.path.join(RESULTS, 's1_off.json'), a.install, ['enable_rows_dist=off'])
    bad = same_as_stock(stock, off)
    allok &= report('G1', not bad, '%d/%d SAME as stock %s' % (n - len(bad), n, bad or ''))

    # G2
    prof = os.path.join(PROFILES, 'lognormal')
    if not os.path.isdir(prof):
        subprocess.run([sys.executable, os.path.join(common.TOOLS, 'profiles.py'),
                        'lognormal:0.5,1.5', prof], check=True, capture_output=True)
    on_sets = ['enable_rows_dist=on', 'error_sample_count=20', 'error_bin_count=2',
               'error_profile_path=' + prof]
    on = dump(os.path.join(RESULTS, 's1_on.json'), a.install, on_sets)
    bad = same_as_stock(stock, on)
    allok &= report('G2', not bad, '%d/%d SAME as stock %s' % (n - len(bad), n, bad or ''))

    # G3
    repeat = 3
    d = dump(os.path.join(RESULTS, 's1_dump.json'), a.install,
             on_sets + ['error_sample_count=13', 'robust_debug_dump=on',
                        'client_min_messages=log'], repeat=repeat)
    problems = []
    for name, q in d['queries'].items():
        ev = parse(q['messages']).get('query', [])
        if len(ev) != 1 + repeat:
            problems.append('%s: %d query events, expected %d' % (name, len(ev), 1 + repeat))
        for e in ev:
            want = {'enabled': True, 'N': 13, 'seed': 42, 'bins': 2, 'bandwidth': 0,
                    'profile_path': prof}
            if any(e.get(k) != v for k, v in want.items()):
                problems.append('%s: %s' % (name, e))
                break
    allok &= report('G3', not problems, '%d queries, %d events each' % (len(d['queries']), 1 + repeat)
                    if not problems else '; '.join(problems[:3]))

    # G4
    common.start(a.install)
    out, err = common.psql('CREATE EXTENSION IF NOT EXISTS test_robust;\n'
                           'SET client_min_messages = log;\nSELECT test_robust_dump();',
                           a.install, on_error_stop=False)
    ev = parse(err.splitlines()).get('test', [])
    if not ev:
        allok &= report('G4', False, 'no test event (is test_robust installed?) ' + err.strip()[-200:])
    else:
        e = ev[0]
        v = e['v']
        checks = [
            e['s'] == 'quote" backslash\\ tab\t é',
            e['null_s'] is None,
            e['i'] == -2 ** 63,
            e['big'] == 123456789012345680.0,
            e['ninf'] == -math.inf,
            e['t'] is True and e['f'] is False,
            v[0] == 0.1 and v[1] == 1.0 / 3.0 and v[2] == 1e-300,
            math.isnan(v[3]) and v[4] == math.inf and v[5] == 0.0,
            e['c'] == [2] and e['null_v'] is None,
        ]
        allok &= report('G4', all(checks), 'exact round trip of %d values' % 9
                        if all(checks) else 'failed checks: %s' % [i for i, c in enumerate(checks) if not c])

    # G5 (informational)
    rows = []
    for name in sorted(stock['queries']):
        ta = stock['queries'][name].get('planning_ms')
        tb = off['queries'][name].get('planning_ms')
        if ta and tb:
            rows.append(tb / ta)
    gm = math.exp(sum(math.log(r) for r in rows) / len(rows))
    report('G5', True, 'planning time robust-off / stock: geometric mean %.3f (min %.2f, max %.2f)'
           % (gm, min(rows), max(rows)))

    common.stop()
    sys.exit(0 if allok else 1)


if __name__ == '__main__':
    main()
