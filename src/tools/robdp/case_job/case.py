"""Case study: one JOB instance, stock vs RobDP with the real (fold-2) error
profile: per-relation row samples against the truth, per-node cost vectors
of both plans, execution times.

    python3 case.py TEMPLATE QUERY_NAME OUT.json [main_objective]
"""
import glob
import itertools
import json
import math
import os
import re
import statistics
import subprocess
import sys

sys.path.insert(0, '/Users/xuanchen2026/RobDP/PG-Robust-v2/src/tools/robdp')
from robdp_dump import parse  # noqa: E402

B = '/Users/xuanchen2026/Projects/RobDP-Backup/'
PSQL = '/Users/xuanchen2026/RobDP/install/v2-release/bin/psql'
N = 20


def psql(text):
    p = subprocess.run([PSQL, '-p', '55501', '-U', 'postgres', '-X', '-q', '-At', '-d', 'imdb'],
                       input=text, capture_output=True, text=True, timeout=900)
    return p.stdout, p.stderr


def robust_sets(t, main=0, dump=False, search=True):
    prof = B + 'imdb-separate-ep-0826-2/%s-0_cardinality/error_profile' % t
    s = [('enable_rows_dist', 'on'), ('error_sample_count', str(N)), ('error_bin_count', '2'),
         ('error_sample_seed', '42'), ('error_sample_kde_bandwidth', '0'), ('error_profile_path', prof),
         ('robust_search', 'on' if search else 'off'), ('robust_select', 'on' if search else 'off'), ('main_objective_id', str(main)),
         ('retain_strategy_id', '0'), ('add_path_limit', '1'), ('retain_path_limit', '1'),
         ('robust_final_path_limit', '1'), ('robust_join_pairing', 'retained'), ('robust_dominance', 'off'),
         ('robust_penalty_tau', '0.2'), ('robust_envelope', 'on')]
    if dump:
        s += [('robust_debug_dump', 'on'), ('client_min_messages', 'log')]
    return ''.join("SET %s = '%s';\n" % kv for kv in s)


PRE = 'SET max_parallel_workers_per_gather = 0;\n'


def explain(sql, sets, analyze=True):
    out, err = psql(PRE + sets + 'EXPLAIN (%sFORMAT JSON) %s;\n'
                    % ('ANALYZE, ' if analyze else '', sql.rstrip().rstrip(';')))
    return json.loads(out)[0], err


def node_list(n, out, depth=0):
    n['_depth'] = depth
    out.append(n)
    for c in n.get('Plans', []):
        node_list(c, out, depth + 1)
    return out


def aliases(n):
    a = {n['Alias']} if 'Alias' in n else set()
    for c in n.get('Plans', []):
        a |= aliases(c)
    return a


# --------------------------------------------------------- the query

def parse_query(sql):
    frm = re.search(r'FROM(.*?)WHERE', sql, re.S).group(1)
    tables = dict((a, t) for t, a in re.findall(r'([a-z_]+) AS ([a-z_0-9]+)', frm))
    where = re.search(r'WHERE(.*)', sql, re.S).group(1).strip().rstrip(';')
    conj = [c.strip() for c in re.split(r'\n\s*AND ', '\n ' + where.strip()) if c.strip()]
    conj = [re.sub(r'^AND ', '', c) for c in conj]
    preds = [(c, set(re.findall(r'\b([a-z_0-9]+)\.', c)) & set(tables)) for c in conj]
    return tables, preds


def connected(s, preds):
    s = set(s)
    seen, todo = set(), [next(iter(s))]
    while todo:
        a = todo.pop()
        if a in seen:
            continue
        seen.add(a)
        for _, al in preds:
            if len(al) == 2 and a in al and al <= s:
                todo += list(al - seen)
    return seen == s


def true_cards(tables, preds):
    out = {}
    names = sorted(tables)
    for k in range(1, len(names) + 1):
        for sub in itertools.combinations(names, k):
            if not connected(sub, preds):
                continue
            ps = [c for c, al in preds if al and al <= set(sub)]
            q = 'SELECT count(*) FROM %s%s;' % (', '.join('%s AS %s' % (tables[a], a) for a in sub),
                                                 (' WHERE ' + ' AND '.join(ps)) if ps else '')
            o, e = psql(q)
            out[' '.join(sorted(sub))] = int(o.strip())
    return out


# ---------------------------------------------------------- vectors

def vec(v):
    if isinstance(v, list) and len(v) == N:
        return [float(x) for x in v]
    x = v[0] if isinstance(v, list) else v
    return [float(x)] * N


def stats(v):
    m = statistics.mean(v)
    sd = statistics.pstdev(v)
    lo, hi = min(v), max(v)
    return dict(min=lo, med=statistics.median(v), max=hi, mean=m, cv=(sd / m if m else 0.0),
                spread=(hi / lo if lo > 0 else float('inf')), varies=hi != lo)


def path_tree(pid, paths, depth=0, out=None):
    out = [] if out is None else out
    p = paths[pid]
    out.append((depth, p))
    for c in p['children']:
        if c is not None and c in paths:
            path_tree(c, paths, depth + 1, out)
    return out


def main():
    t, name, outf = sys.argv[1], sys.argv[2], sys.argv[3]
    main_obj = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    qs = json.load(open(glob.glob(B + 'imdb-separate-ep-0826-1/%s-0_cardinality/raw_data/*.json' % t)[0]))
    sql = qs[name]
    tables, preds = parse_query(sql)
    res = dict(sql=sql, template=t, name=name, objective=main_obj)

    # truth
    res['true'] = true_cards(tables, preds)

    # stock and RobDP, executed (median of 5 after a warm-up)
    for tag, sets in (('pg', ''), ('robdp', robust_sets(t, main_obj))):
        explain(sql, sets)
        runs = [explain(sql, sets)[0] for _ in range(5)]
        r = runs[0]
        res[tag] = dict(ms=statistics.median(x['Execution Time'] for x in runs),
                        ms_all=[x['Execution Time'] for x in runs],
                        planning_ms=statistics.median(x['Planning Time'] for x in runs), plan=r['Plan'])

    # RobDP's planning, dumped
    _, err = explain(sql, robust_sets(t, main_obj, dump=True), analyze=False)
    ev = parse([l for l in err.splitlines() if 'ROBDP ' in l])
    first_q = 0
    res['draws'] = ev.get('draw', [])
    res['rels'] = [e for e in ev.get('rel', []) if e.get('pass') in ('base', 'main') and e.get('first', True)]
    res['final'] = [e for e in ev.get('final', [])][-1]
    paths = {e['id']: e for e in ev.get('path', []) if e.get('pass') in ('main', 'upper', 'base')}
    res['paths_chosen'] = [(d, p) for d, p in path_tree(res['final']['chosen'], paths)]
    res['paths_pg'] = [(d, p) for d, p in path_tree(res['final']['pg'], paths)]
    env = [e for e in ev.get('env', [])]
    res['env'] = {e['rels']: e['cost'] for e in env}

    # stock PostgreSQL's plan with its vectors: robust_search off plans as stock
    _, err = explain(sql, robust_sets(t, main_obj, dump=True, search=False), analyze=False)
    ev2 = parse([l for l in err.splitlines() if 'ROBDP ' in l])
    paths2 = {e['id']: e for e in ev2.get('path', [])}
    top = [e for e in ev2.get('final', [])]
    # without robust_select there is no final event: the plan's top path is the
    # last path of the final upper relation
    finals = [e for e in ev2.get('path', []) if e.get('upper')]
    res['stock_top'] = finals[-1]['id'] if finals else None
    res['paths_stock'] = [(d, p) for d, p in path_tree(res['stock_top'], paths2)] if finals else []
    json.dump(res, open(outf, 'w'), indent=1, default=str)
    print('saved', outf)


if __name__ == '__main__':
    main()
