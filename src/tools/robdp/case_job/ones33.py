"""All-ones profiles on template 33 (14 relations) with GEQO off, so that
RobDP's join search runs: stock with GEQO on and off, RobDP k = 1+0 and 8+0
for a few objectives.  One execution each (stock: warm-up first).

    ONES=dir OUT=ones33.json python3 ones33.py
"""
import glob, json, os
from cmp_runs import B, robust, explain, shape

ONES = os.environ['ONES']
qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/33-0_cardinality/raw_data/*.json')[0]))
TO = "SET statement_timeout = '300s';\n"
out = {}
for name, sql in list(qs.items())[:10]:
    cfgs = [('pg geqo', TO), ('pg dp', "SET geqo = off;\n" + TO)]
    for add in (1, 8):
        for o in (3, 0, 8):
            cfgs.append(('k%d o%d' % (add, o), robust('33', o, rlimit=0, extra=[
                ('error_profile_path', os.path.join(ONES, '33')), ('add_path_limit', str(add)), ('geqo', 'off')]) + TO))
    row = {}
    for tag, st in cfgs:
        if tag.startswith('pg'):
            explain(sql, st)
        r = explain(sql, st)
        row[tag] = dict(ms=r['Execution Time'], plan_ms=r['Planning Time'], cost=r['Plan']['Total Cost'],
                        shape=shape(r['Plan']))
    out[name] = row
    print('%-16s ' % name + ' | '.join('%s %.1f+%.0f%s' % (k, c['ms'], c['plan_ms'], '=' if c['shape'] == row['pg dp']['shape'] else '*')
                                     for k, c in row.items()), flush=True)
    json.dump(out, open(os.environ['OUT'], 'w'), indent=1)
