"""How many Gather candidates does a join relation's complete selection see,
and would a Gather on a partial path the partial selection dropped make the
complete selection keep something else?  v2 dumps, main pass.

A plain Gather on partial path P costs P's total plus an overhead that does
not depend on P (cost_gather: setup + parallel_tuple_cost x the rel's rows),
so the hypothetical Gather(P) has total(s) = P.total(s) + G.total(s) -
Q.total(s), G the real plain Gather and Q its partial path.  Only plain
Gathers (no Gather Merge), complete selections of join relations.

    ROBDP_PAR=8 ROBDP_PAR_GUCS=... python3 gathercount.py OBJ PER TEMPLATE ...
"""
import collections, glob, json, re, subprocess, sys
sys.path.insert(0, '/Users/xuanchen2026/RobDP/PG-Robust-v2/src/tools/robdp')
from robdp_dump import parse
import objectives
from cmp_runs import B, PSQL, PARSET, robust

N = 20
OBJ, PER = int(sys.argv[1]), int(sys.argv[2])


def ntables(sql):
    return len(re.findall(r'\b[a-z_]+ AS [a-z_0-9]+', sql.split('FROM', 1)[1].split('WHERE')[0]))


def dump(t, sql):
    q = (robust(t, OBJ, rlimit=0) + PARSET + "SET robust_debug_dump = 'on';\nSET client_min_messages = 'log';\n"
         "EXPLAIN (FORMAT JSON) %s;\n" % sql.rstrip().rstrip(';'))
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=q, capture_output=True, text=True, timeout=900)
    return parse([l for l in p.stderr.splitlines() if 'ROBDP ' in l])


tot = collections.Counter()
for t in sys.argv[3:]:
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    for name, sql in list(qs.items())[:PER]:
        if ntables(sql) >= 12:
            continue
        ev = dump(t, sql)
        paths = {e['id']: e for e in ev.get('path', [])}
        env = {e['rels']: e for e in ev.get('env', [])}
        keeps = [k for k in ev.get('keep', []) if k.get('pass') == 'main' and k['limit'] < len(k['cands'])]
        part = {k['rels']: k for k in keeps if k['partial']}
        c = collections.Counter()
        for k in keeps:
            if k['partial'] or k['rels'] not in part:
                continue
            pk = part[k['rels']]
            cands = [paths[i] for i in k['cands']]
            gathers = [p for p in cands if p['type'] == 'Gather']
            gms = [p for p in cands if p['type'] == 'GatherMerge']
            c['join rels with partial paths'] += 1
            c['partial candidates'] += len(pk['cands'])
            c['partial kept'] += len(pk['kept'])
            c['plain Gather candidates'] += len(gathers)
            c['Gather Merge candidates'] += len(gms)
            c['complete candidates'] += len(cands)
            if len(gathers) != 1:
                continue
            g = gathers[0]
            q = paths[g['children'][0]]
            gt, qt = objectives.vec(g['total'], N), objectives.vec(q['total'], N)
            over = [gt[s] - qt[s] for s in range(N)]
            e = env.get(k['rels'])
            envc = objectives.vec(e['cost'], N) if e and e.get('cost') is not None else None
            envc, _ = objectives.selection_envelope(cands, False, envc, objectives.needs_env(OBJ), N)
            kept_worst = max(objectives.objective(OBJ, paths[i], envc, 0.2) for i in k['kept'][:k['limit']])
            best_kept = objectives.objective(OBJ, paths[k['kept'][0]], envc, 0.2)
            for pid in pk['cands']:
                if pid in pk['kept']:
                    continue
                pt = objectives.vec(paths[pid]['total'], N)
                hyp = dict(paths[pid], total=[pt[s] + over[s] for s in range(N)], param=None)
                sc = objectives.objective(OBJ, hyp, envc, 0.2)
                if sc < best_kept - 1e-9 * max(1.0, abs(best_kept)):
                    c['dropped partial whose Gather would beat the kept best'] += 1
                    c['  rels affected'] += 0
                    break
        tot.update(c)
        print(name, dict(c), flush=True)
print('TOTAL', dict(tot))
