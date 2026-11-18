#!/usr/bin/env python3
"""
Compare two explain_dump.py outputs, normally a stock baseline (A) and a
candidate (B).

    compare.py A.json B.json [--cost-tol 1e-9] [--verbose]

Per query:
  SAME       same plan shape, every node's rows equal and costs equal
  COST-DIFF  same shape and rows, some cost differs beyond tolerance
  ROWS-DIFF  same shape, some node's row estimate differs
  PLAN-DIFF  different plan shape (node types, relations, join order)
  ERROR      one side failed

EXPLAIN prints costs with two decimals, so the cost tolerance is
max(0.005 + 1e-12, cost-tol * |cost|): equal printed values pass.  Exact
(17-digit) comparisons use the RobDP dump lines instead.

Exit status is 0 only if every query is SAME (use it as a test gate).
"""
import argparse
import json
import sys

SHAPE_KEYS = ('Node Type', 'Join Type', 'Relation Name', 'Alias', 'Strategy',
              'Parent Relationship', 'Index Name', 'Scan Direction')


def shape(node):
    return (tuple(node.get(k) for k in SHAPE_KEYS),
            tuple(shape(c) for c in node.get('Plans', [])))


def nodes(node, path='0'):
    yield path, node
    for i, c in enumerate(node.get('Plans', [])):
        yield from nodes(c, path + '.' + str(i))


def label(node):
    s = node['Node Type']
    if 'Alias' in node:
        s += ' ' + node['Alias']
    return s


def cost_close(x, y, tol):
    return abs(x - y) <= max(0.005 + 1e-12, tol * max(abs(x), abs(y)))


def compare_plans(pa, pb, tol):
    if shape(pa) != shape(pb):
        return 'PLAN-DIFF', []
    rows_diff, cost_diff = [], []
    for (path, na), (_, nb) in zip(nodes(pa), nodes(pb)):
        if na['Plan Rows'] != nb['Plan Rows']:
            rows_diff.append('%s %s rows %s -> %s' % (path, label(na), na['Plan Rows'], nb['Plan Rows']))
        for k in ('Startup Cost', 'Total Cost'):
            if not cost_close(na[k], nb[k], tol):
                cost_diff.append('%s %s %s %s -> %s' % (path, label(na), k, na[k], nb[k]))
    if rows_diff:
        return 'ROWS-DIFF', rows_diff + cost_diff
    if cost_diff:
        return 'COST-DIFF', cost_diff
    return 'SAME', []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('--cost-tol', type=float, default=1e-9)
    ap.add_argument('--verbose', '-v', action='store_true')
    args = ap.parse_args()
    A = json.load(open(args.a))
    B = json.load(open(args.b))

    counts = {}
    print('A: %s %s' % (A['install'], ' '.join(A['settings'])))
    print('B: %s %s' % (B['install'], ' '.join(B['settings'])))
    print('%-20s %-10s %10s %10s' % ('query', 'result', 'plan A ms', 'plan B ms'))
    for name in sorted(set(A['queries']) | set(B['queries'])):
        qa, qb = A['queries'].get(name), B['queries'].get(name)
        if not qa or not qb or 'plan' not in qa or 'plan' not in qb:
            verdict, detail = 'ERROR', [(qb or {}).get('error', '') or (qa or {}).get('error', 'missing')]
        else:
            verdict, detail = compare_plans(qa['plan'], qb['plan'], args.cost_tol)
        counts[verdict] = counts.get(verdict, 0) + 1
        ta = (qa or {}).get('planning_ms')
        tb = (qb or {}).get('planning_ms')
        print('%-20s %-10s %10s %10s' % (name, verdict,
                                          '%.2f' % ta if ta else '-', '%.2f' % tb if tb else '-'))
        if args.verbose or verdict == 'ERROR':
            for d in detail[:8]:
                print('      ' + d)
    print('summary: ' + ', '.join('%s %d' % kv for kv in sorted(counts.items())))
    sys.exit(0 if set(counts) == {'SAME'} else 1)


if __name__ == '__main__':
    main()
