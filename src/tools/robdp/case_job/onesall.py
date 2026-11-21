"""All-ones profiles on every fold-1 query of the given templates:
stock vs RobDP E[Cost] at k=1 and k=8 (retained pairing). EXPLAIN for
point cost and plan shape; for plans that differ from stock, median of 3
warm executions of each (interleaved).

    ONES=dir-with-<T>/ OUT=onesall.json python3 onesall.py TEMPLATE ...
"""
import glob, json, os, re, statistics, sys
from cmp_runs import B, robust, explain, shape

ONES = os.environ['ONES']

def sets(t, k):
    return robust(t, 3, extra=[('error_profile_path', os.path.join(ONES, t)), ('add_path_limit', str(k)),
                               ('retain_path_limit', '0')]) + "SET statement_timeout = '30s';\n"

def ntables(sql):
    return len(re.findall(r'\b[a-z_]+ AS [a-z_0-9]+', sql.split('FROM', 1)[1].split('WHERE')[0]))

out = {}
for t in sys.argv[1:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in qs.items():
        if ntables(sql) >= 12:
            continue
        cfg = {'pg': '', 'k1': sets(t, 1), 'k8': sets(t, 8)}
        row = {}
        for tag, st in cfg.items():
            r = explain(sql, st, analyze=False)
            row[tag] = dict(cost=r['Plan']['Total Cost'], shape=shape(r['Plan']))
        diff = [tag for tag in ('k1', 'k8') if row[tag]['shape'] != row['pg']['shape']]
        if diff:
            tags = ['pg'] + [d for d in diff if not (d == 'k8' and 'k1' in diff and row['k8']['shape'] == row['k1']['shape'])]
            ms = {tag: [] for tag in tags}
            try:
                for tag in tags:
                    explain(sql, cfg[tag])
                for _ in range(3):
                    for tag in tags:
                        ms[tag].append(explain(sql, cfg[tag])['Execution Time'])
                for tag in tags:
                    row[tag]['ms'] = statistics.median(ms[tag])
                if 'k8' in diff and 'k8' not in tags:
                    row['k8']['ms'] = row['k1']['ms']
            except Exception as e:
                row['err'] = str(e)[-200:]
        out[name] = row
        print('%-16s %s' % (name, ' | '.join('%s %.4f%s' % (tag, row[tag]['cost'] / row['pg']['cost'],
              (' %.1f/%.1fms' % (row[tag]['ms'], row['pg']['ms'])) if 'ms' in row[tag] else '')
              for tag in ('k1', 'k8'))), flush=True)
        json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
