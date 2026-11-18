#!/usr/bin/env python3
"""
Plan every benchmark query with one build and one set of GUCs, and save the
plans.

    explain_dump.py OUT.json [--install v2-release] [--set name=value ...]
                    [--queries DIR] [--repeat 5]

For each query: EXPLAIN (FORMAT JSON) once for the plan, then --repeat more
times for the planning time (median).  Server messages (LOG lines included,
when client_min_messages = log) are kept per query in "messages", so the
RobDP dump lines (ROBDP_DUMP_SPEC.md) can be parsed from the same file.

Settings are applied in the order given, after max_parallel_workers_per_gather
= 0 (parallel plans are studied separately).
"""
import argparse
import json
import re
import statistics

import common


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--install', default='v2-release')
    ap.add_argument('--set', action='append', default=[])
    ap.add_argument('--queries')
    ap.add_argument('--repeat', type=int, default=5)
    a = ap.parse_args()

    common.start(a.install)
    sets = common.parse_sets(['max_parallel_workers_per_gather=0'] + a.set)
    result = {'install': a.install, 'settings': a.set, 'queries': {}}
    for name, q in common.load_queries(a.queries):
        sql = sets + '\nEXPLAIN (FORMAT JSON) %s;\n' % q
        sql += ('EXPLAIN (SUMMARY ON, COSTS OFF) %s;\n' % q) * a.repeat
        out, err = common.psql(sql, a.install, on_error_stop=False)
        entry = {'sql': q, 'messages': err.splitlines()}
        m = re.search(r'^\[\s*\{.*?^\]$', out, re.S | re.M)
        if m:
            entry['plan'] = json.loads(m.group(0))[0]['Plan']
            times = [float(t) for t in re.findall(r'Planning Time: ([\d.]+) ms', out)]
            entry['planning_ms'] = statistics.median(times) if times else None
        else:
            entry['error'] = err.strip()[-2000:]
        result['queries'][name] = entry
        status = 'ERROR' if 'error' in entry else '%.2f ms' % (entry['planning_ms'] or 0)
        print('%-20s %s' % (name, status))
    with open(a.out, 'w') as f:
        json.dump(result, f, indent=1)


if __name__ == '__main__':
    main()
