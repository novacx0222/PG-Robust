"""stock vs RobDP with 2 and 4 bins (max Penalty and E[Cost]), same
instances as cmp_runs.py"""
import json, os, sys, glob, statistics
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cmp_runs import B, robust, timed, shape
out = {}
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:10]:
        row = {}
        for tag, sets in (('pg', ''), ('r0b2', robust(t, 0)), ('r0b4', robust(t, 0, extra=[('error_bin_count', '4')])),
                          ('r3b2', robust(t, 3)), ('r3b4', robust(t, 3, extra=[('error_bin_count', '4')]))):
            timed(sql, sets, 1)
            ms, r = timed(sql, sets)
            row[tag] = dict(ms=ms, plan=shape(r['Plan']))
        out[name] = row
        pg = row['pg']['ms']
        print('%-15s pg %8.1f | ' % (name, pg) + ' | '.join('%s %8.1f %5.2fx' % (k, row[k]['ms'], pg / row[k]['ms'])
                                                          for k in ('r0b2', 'r0b4', 'r3b2', 'r3b4')), flush=True)
json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
