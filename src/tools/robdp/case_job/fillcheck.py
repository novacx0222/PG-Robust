"""Envelope scenarios a selection takes from its candidates (keep events'
`filled`), on JOB with parallel query on: count them and recompute every
main-pass keep event's main scores exactly (objectives.selection_envelope).

    python3 fillcheck.py OBJ TEMPLATE ...
"""
import glob, json, sys, collections, subprocess
sys.path.insert(0, '/Users/xuanchen2026/RobDP/PG-Robust-v2/src/tools/robdp')
from robdp_dump import parse
import objectives
from cmp_runs import B, robust, PSQL

N = 20
o = int(sys.argv[1])
tot, bad = collections.Counter(), []
for t in sys.argv[2:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:4]:
        q = (robust(t, o) + "SET max_parallel_workers_per_gather = 2;\nSET robust_debug_dump = 'on';\n"
             "SET client_min_messages = 'log';\nEXPLAIN (FORMAT JSON) %s;\n" % sql.rstrip().rstrip(';'))
        p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                           input=q, capture_output=True, text=True, timeout=900)
        if p.returncode != 0:
            raise RuntimeError(p.stderr[-300:])
        ev = parse([l for l in p.stderr.splitlines() if 'ROBDP ' in l])
        paths = {e['id']: e for e in ev.get('path', [])}
        env = {e['rels']: e for e in ev.get('env', [])}
        tot['queries'] += 1
        for k in ev.get('keep', []):
            if k.get('pass') != 'main':
                continue
            tot['keeps'] += 1
            f = k.get('filled', 0)
            if f:
                tot['filled partial' if k['partial'] else 'filled complete'] += 1
                tot['filled scenarios'] += f
            cands = [paths[i] for i in k['cands']]
            e = env.get(k['rels'])
            evec = objectives.vec(e['partial' if k['partial'] else 'cost'], N) if k['envelope'] and e else None
            evec, ff = objectives.selection_envelope(cands, k['partial'], evec, objectives.needs_env(o), N)
            if ff != f:
                bad.append(('filled', name, k['rels'], f, ff))
                continue
            if [objectives.objective(o, pp, evec, 0.2) for pp in cands] != k['main']:
                bad.append(('main', name, k['rels'], k['partial']))
                continue
            tot['recomputed exactly'] += 1
print('objective', o, dict(tot))
print('problems', len(bad), bad[:5])
