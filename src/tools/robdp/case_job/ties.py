"""How often the main objective's best score is tied in a selection (keep
events of the main pass), e.g. E[Penalty] = 0 for several candidates, so
that the tie-break (mean total, point total, point startup, position)
decides.  Also: selections without an envelope.

    OBJ=1 python3 ties.py TEMPLATE ...
"""
import glob, json, os, sys, collections
sys.path.insert(0, '/Users/xuanchen2026/RobDP/PG-Robust-v2/src/tools/robdp')
from robdp_dump import parse
from cmp_runs import B, robust
import case

OBJ = int(os.environ.get('OBJ', '1'))
tot = collections.Counter()
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:10]:
        st = robust(t, OBJ) + "SET robust_debug_dump = 'on';\nSET client_min_messages = 'log';\n"
        _, err = case.explain(sql, st, analyze=False)
        ev = parse([l for l in err.splitlines() if 'ROBDP ' in l])
        c = collections.Counter()
        for e in ev.get('keep', []):
            if e.get('pass') != 'main':
                continue
            m = e['main']
            n = len(m)
            if n < 2 or e['limit'] >= n:
                continue        # scored only or nothing to choose
            k = 'partial' if e['partial'] else 'complete'
            c[k + ' sel'] += 1
            if not e['envelope']:
                c[k + ' noenv'] += 1
            best = min(m)
            nbest = sum(1 for x in m if x == best)
            if nbest > 1:
                c[k + ' tie'] += 1
                if best == 0:
                    c[k + ' tie0'] += 1
            if all(x == 0 for x in m):
                c[k + ' all0'] += 1
        tot.update(c)
        print('%-16s %s' % (name, dict(c)), flush=True)
print('TOTAL', dict(tot))
