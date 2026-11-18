#!/usr/bin/env python3
"""
Parser for the RobDP debug dump (ROBDP_DUMP_SPEC.md).

    from robdp_dump import parse
    events = parse(lines)            # lines: psql stderr or explain_dump "messages"
    events['rel']                    # list of dicts, in emission order

As a script, prints a per-event-type count for an explain_dump.py output:

    robdp_dump.py DUMP.json [query]
"""
import json
import math
import re
import sys

_LINE = re.compile(r'^(?:LOG|DEBUG\d?):\s+ROBDP (\{.*\})\s*$')
_SPECIAL = {'nan': math.nan, 'inf': math.inf, '-inf': -math.inf}


def _fix(v):
    if isinstance(v, str) and v in _SPECIAL:
        return _SPECIAL[v]
    if isinstance(v, list):
        return [_fix(x) for x in v]
    if isinstance(v, dict):
        return {k: _fix(x) for k, x in v.items()}
    return v


def parse(lines):
    events = {}
    for line in lines:
        m = _LINE.match(line.strip())
        if not m:
            continue
        obj = _fix(json.loads(m.group(1)))
        events.setdefault(obj.get('ev', '?'), []).append(obj)
    return events


def vec(v, n):
    """Broadcast a length-1 vector to length n (spec: length is 1 or N)."""
    if len(v) == 1:
        return v * n
    if len(v) != n:
        raise ValueError('vector of length %d, expected 1 or %d' % (len(v), n))
    return v


def _selftest():
    lines = [
        'LOG:  ROBDP {"ev":"rel","pass":0,"rels":"t1","kind":"base","rows_point":10,"rows":[10,20]}',
        'LOG:  ROBDP {"ev":"env","rels":"t1 t2","s":0,"cost":"inf"}',
        'NOTICE:  something else',
    ]
    ev = parse(lines)
    assert ev['rel'][0]['rows'] == [10, 20]
    assert math.isinf(ev['env'][0]['cost'])
    assert vec([3.0], 4) == [3.0] * 4
    print('robdp_dump self-test ok')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        _selftest()
        sys.exit(0)
    d = json.load(open(sys.argv[1]))
    for name, q in d['queries'].items():
        if len(sys.argv) > 2 and name != sys.argv[2]:
            continue
        ev = parse(q.get('messages', []))
        print('%-20s %s' % (name, ', '.join('%s %d' % (k, len(v)) for k, v in sorted(ev.items())) or '-'))
