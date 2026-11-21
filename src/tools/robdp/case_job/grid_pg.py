"""Stock re-timed on a warm cache for grid.py's queries (grid.py runs stock
first, cold): one warm-up, then one timed execution."""
import glob, json, os, sys
from cmp_runs import B, explain
g = json.load(open(sys.argv[1]))
for name in g:
    t = name.split('-')[0]
    sql = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))[name]
    explain(sql, '')
    r = explain(sql, '')
    g[name]['pg_warm'] = dict(ms=r['Execution Time'], plan_ms=r['Planning Time'])
json.dump(g, open(sys.argv[1], 'w'), indent=1)
