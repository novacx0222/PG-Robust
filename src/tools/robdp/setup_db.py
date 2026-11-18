#!/usr/bin/env python3
"""
Create (or recreate) the benchmark database "robdp" on the v2 bench cluster.

    setup_db.py [--install v2-release] [--recreate]

The schema is deterministic (see schema.sql), so recreating it gives the same
statistics.  Prints a fingerprint of pg_statistic so two loads can be
compared.
"""
import argparse
import os

import common


def fingerprint(install):
    out, _ = common.psql(
        # everything but the OIDs, which change when the database is recreated
        "SELECT md5(string_agg(concat_ws(',', c.relname, s.staattnum, s.stainherit, "
        "s.stanullfrac, s.stawidth, s.stadistinct, s.stakind1, s.stakind2, s.stakind3, "
        "s.stakind4, s.stakind5, s.stanumbers1, s.stanumbers2, s.stanumbers3, "
        "s.stanumbers4, s.stanumbers5, s.stavalues1::text, s.stavalues2::text, "
        "s.stavalues3::text, s.stavalues4::text, s.stavalues5::text, c.reltuples, "
        "c.relpages), '|' ORDER BY c.relname, s.staattnum, s.stainherit)) "
        "FROM pg_statistic s JOIN pg_class c ON c.oid = s.starelid "
        "WHERE c.relnamespace = 'public'::regnamespace;", install)
    return out.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--install', default='v2-release')
    ap.add_argument('--recreate', action='store_true')
    a = ap.parse_args()

    common.start(a.install)
    out, _ = common.psql("SELECT 1 FROM pg_database WHERE datname = 'robdp';",
                         a.install, db='postgres')
    if out.strip() and a.recreate:
        common.psql('DROP DATABASE robdp;', a.install, db='postgres')
        out = ''
    if not out.strip():
        common.psql('CREATE DATABASE robdp;', a.install, db='postgres')
        common.psql(open(os.path.join(common.TOOLS, 'schema.sql')).read(), a.install)
    print('statistics fingerprint:', fingerprint(a.install))


if __name__ == '__main__':
    main()
