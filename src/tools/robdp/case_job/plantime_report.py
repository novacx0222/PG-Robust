"""Report for plantime.py: per query and median over the 13 queries, v2's
configurations next to v20's.

    python3 plantime_report.py plantime_v2.json plantime_v20.json
"""
import json, statistics, sys

v2 = json.load(open(sys.argv[1]))
v20 = json.load(open(sys.argv[2])) if len(sys.argv) > 2 else {}
names = list(v2)
for sfx, title in (('', 'max parallel'), ('/off', 'parallel off')):
    print('== %s: planning ms (median of EXPLAINs 2..4)' % title)
    cols = [('stock', 'v2', 'stock'), ('v20 c1', 'v20', 'c1'), ('v2 c1', 'v2', 'c1'), ('v2 c1 nat', 'v2', 'c1 native'),
            ('v20 c1 N1', 'v20', 'c1 N1'), ('v2 c1 nat N1', 'v2', 'c1 native N1'), ('v2 c1 N1', 'v2', 'c1 N1'),
            ('v20 p8', 'v20', 'p8'), ('v2 p8', 'v2', 'p8'), ('v2 p8 nat', 'v2', 'p8 native'),
            ('v20 p8 N1', 'v20', 'p8 N1'), ('v2 p8 nat N1', 'v2', 'p8 native N1'), ('v2 p8 N1', 'v2', 'p8 N1')]
    cols = [c for c in cols if (v2 if c[1] == 'v2' else v20).get(names[0], {}).get(c[2] + sfx) is not None]
    print('%-16s' % 'query' + ''.join('%13s' % c[0] for c in cols))
    for n in names:
        print('%-16s' % n + ''.join('%13.1f' % (v2 if c[1] == 'v2' else v20)[n][c[2] + sfx] for c in cols))
    print('%-16s' % 'median' + ''.join('%13.1f' % statistics.median((v2 if c[1] == 'v2' else v20)[n][c[2] + sfx]
                                                                  for n in names) for c in cols))
    print()
