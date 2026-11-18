"""
Shared helpers for the RobDP v2 test harness.

Every script talks to one benchmark cluster (data directory DATA, port PORT)
that can be started with any installed build under RobDP/install/<name>.
All builds are PostgreSQL 16.2 with the same catalog version, so the same
data directory -- and therefore the same statistics -- is reused by all of
them.  That is what makes plan and cost comparisons across builds exact.
"""
import os
import re
import subprocess
import time

ROOT = '/Users/xuanchen2026/RobDP'
DATA = os.path.join(ROOT, 'data', 'v2-bench')
PORT = 55501
TOOLS = os.path.dirname(os.path.abspath(__file__))


def bindir(install):
    return os.path.join(ROOT, 'install', install, 'bin')


def running_install():
    """Name of the install whose postmaster owns DATA, or None."""
    pidfile = os.path.join(DATA, 'postmaster.pid')
    if not os.path.exists(pidfile):
        return None
    marker = os.path.join(DATA, 'robdp_install')
    return open(marker).read().strip() if os.path.exists(marker) else '?'


def stop():
    inst = running_install()
    if inst is None:
        return
    pg_ctl = os.path.join(bindir(inst if inst != '?' else 'v2-release'), 'pg_ctl')
    subprocess.run([pg_ctl, '-D', DATA, '-m', 'fast', '-w', 'stop'],
                   check=True, capture_output=True)


def start(install):
    """Start the cluster with the given build (restarting if another runs)."""
    if running_install() == install:
        return
    stop()
    b = bindir(install)
    if not os.path.exists(os.path.join(DATA, 'PG_VERSION')):
        subprocess.run([os.path.join(b, 'initdb'), '-D', DATA, '-A', 'trust',
                        '-U', 'postgres', '--no-locale', '-E', 'UTF8'],
                       check=True, capture_output=True)
    subprocess.run([os.path.join(b, 'pg_ctl'), '-D', DATA, '-w',
                    '-l', os.path.join(DATA, 'server.log'),
                    '-o', '-p %d -c shared_buffers=256MB -c max_connections=20' % PORT,
                    'start'], check=True, capture_output=True)
    with open(os.path.join(DATA, 'robdp_install'), 'w') as f:
        f.write(install)


def psql(sql, install, db='robdp', on_error_stop=True):
    """Run SQL through psql; returns (stdout, stderr).  LOG/NOTICE go to stderr."""
    cmd = [os.path.join(bindir(install), 'psql'), '-p', str(PORT), '-U', 'postgres',
           '-X', '-q', '-At', '-d', db]
    if on_error_stop:
        cmd += ['-v', 'ON_ERROR_STOP=1']
    r = subprocess.run(cmd, input=sql, capture_output=True, text=True)
    if on_error_stop and r.returncode != 0:
        raise RuntimeError('psql failed (%d):\n%s' % (r.returncode, r.stderr[-4000:]))
    return r.stdout, r.stderr


def parse_sets(pairs):
    """['a=1', 'b=x'] -> SQL SET statements (values passed through set_config)."""
    out = []
    for p in pairs or []:
        name, _, value = p.partition('=')
        out.append("SELECT set_config('%s', '%s', false);" % (name.strip(),
                                                              value.strip().replace("'", "''")))
    return '\n'.join(out)


def load_queries(qdir=None):
    qdir = qdir or os.path.join(TOOLS, 'queries')
    qs = []
    for fn in sorted(os.listdir(qdir)):
        if fn.endswith('.sql'):
            text = open(os.path.join(qdir, fn)).read()
            text = re.sub(r'--[^\n]*', '', text).strip().rstrip(';')
            qs.append((fn[:-4], text))
    return qs
