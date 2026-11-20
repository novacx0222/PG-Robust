"""
RobDP's objectives and diversification functions (util/robustselect.c),
recomputed with the same operations in the same order, so that the gates can
compare the dumped scores and picks exactly.

Objectives (lower is better), v20's numbering, x(s) the loss in scenario s:

    0  max Penalty          10  max Penalty + lambda Stdev
    1  E[Penalty]           11  E[Penalty] + lambda Stdev
    2  max total            12  Quant_alpha[Penalty]
    3  E[Cost]              17  Quant_alpha[total]
    4  max startup          18  Quant_alpha[startup]
    5  E[startup]
    6  1 - ProbNoPenalty  (E of the indicator Penalty(s) > 0)
    8  PostgreSQL's point estimate of the total cost

Diversification functions (retain strategy only): 7 Rand, 13 Similarity,
14 Mix, 15 E[Penalty]-Join, 16 E[Cost]-Join.
"""
import math

OBJECTIVES = (0, 1, 2, 3, 4, 5, 6, 8, 10, 11, 12, 17, 18)
DIVERSIFIERS = (7, 13, 14, 15, 16)
MIX = (3, 1, 11, 12, 6, 13, 16)
NAMES = {0: 'max Penalty', 1: 'E[Penalty]', 2: 'max Cost', 3: 'E[Cost]', 4: 'max startup', 5: 'E[startup]',
         6: '1-ProbNoPenalty', 7: 'Rand', 8: 'point cost', 10: 'max Penalty+Stdev', 11: 'E[Penalty]+Stdev',
         12: 'Quant[Penalty]', 13: 'Similarity', 14: 'Mix', 15: 'E[Penalty]-Join', 16: 'E[Cost]-Join',
         17: 'Quant[Cost]', 18: 'Quant[startup]'}

_LOSS = {6: 'excess', 2: 'total', 3: 'total', 17: 'total', 4: 'startup', 5: 'startup', 18: 'startup',
         8: 'point'}
_AGG = {0: 'max', 2: 'max', 4: 'max', 10: 'max_std', 11: 'mean_std', 12: 'quantile', 17: 'quantile',
        18: 'quantile'}


def loss(oid):
    return _LOSS.get(oid, 'penalty')


def agg(oid):
    return _AGG.get(oid, 'mean')


def needs_env(oid):
    if oid in DIVERSIFIERS:
        return oid not in (7, 16)
    return oid in OBJECTIVES and loss(oid) in ('penalty', 'excess')


def vec(v, n):
    v = v if isinstance(v, list) and len(v) == n else [v[0] if isinstance(v, list) else v] * n
    return [float(x) for x in v]


def mean(v):
    s = 0.0
    for x in v:
        s += x
    return s / len(v)


def aggregate(how, x, lam=1.0, alpha=0.85):
    """robustselect.c aggregate()"""
    n = len(x)
    m, s = 0.0, 0.0
    for v in x:
        s += v
        if v > m:
            m = v
    mu = s / n
    if how == 'max':
        return m
    if how == 'mean':
        return mu
    if how in ('mean_std', 'max_std'):
        ss = 0.0
        for v in x:
            ss += (v - mu) * (v - mu)
        return (mu if how == 'mean_std' else m) + lam * math.sqrt(ss / n)
    k = max(1, min(n, math.ceil(alpha * n - 1e-9)))
    return sorted(x)[k - 1]


def loss_at(how, cost, startup, env, tau):
    """robustselect.c loss_at()"""
    if how in ('penalty', 'excess'):
        if env is None or math.isnan(env) or not (cost > env * (1.0 + tau)):
            return 0.0
        return cost - env if how == 'penalty' else 1.0
    if how == 'startup':
        return startup
    return cost


def objective(oid, p, env, tau, lam=1.0, alpha=0.85, N=20):
    """robustselect.c objective(): p a path event, env the envelope or None"""
    how = loss(oid)
    if how == 'point':
        return p['total_point']
    t, st = vec(p['total'], N), vec(p['startup'], N)
    x = [loss_at(how, t[s], st[s], None if env is None else env[s], tau) for s in range(N)]
    return aggregate(agg(oid), x, lam, alpha)


# --------------------------------------------- diversification functions

def _kl(p, q):
    sp, sq = 0.0, 0.0
    for a, b in zip(p, q):
        sp += a
        sq += b
    if sp <= 0.0 or sq <= 0.0:
        return 0.0
    kl = 0.0
    for a, b in zip(p, q):
        pi = a / sp + 1e-10
        qi = b / sq + 1e-10
        kl += pi * math.log(pi / qi)
    return kl


def js_distance(p, q):
    m = [0.5 * (a + b) for a, b in zip(p, q)]
    d = 0.5 * _kl(p, m) + 0.5 * _kl(q, m)
    return math.sqrt(max(d, 0.0))


class _Member:
    def __init__(self, did, cands, rest, taken, env, cfg, ops, draws, N):
        self.did, self.rest, self.taken, self.N = did, rest, taken, N
        tau, lam, alpha = cfg['tau'], cfg.get('lam', 1.0), cfg.get('alpha', 0.85)
        rk = [rest_key(cands[i], N) for i in rest]
        n = len(rest)
        self.next = 0
        if did in OBJECTIVES:
            key = [objective(did, cands[i], env, tau, lam, alpha, N) for i in rest]
            self.order = sorted(range(n), key=lambda j: (key[j],) + rk[j])
        elif did == 7:
            self.order = sorted(range(n), key=lambda j: (draws[j],) + rk[j])
        elif did in (15, 16):
            obj = 1 if did == 15 else 3
            key = [objective(obj, cands[i], env, tau, lam, alpha, N) for i in rest]
            srt = sorted(range(n), key=lambda j: (key[j],) + rk[j])
            gk, grp = [], []
            for j in srt:
                op = ops[rest[j]]
                if op not in gk:
                    gk.append(op)
                grp.append(gk.index(op))
            out, rnd = [], 0
            while len(out) < n:
                for g in range(len(gk)):
                    seen = 0
                    for jj, j in enumerate(srt):
                        if grp[jj] != g:
                            continue
                        if seen == rnd:
                            out.append(j)
                            break
                        seen += 1
                rnd += 1
            self.order = out
        elif did == 13:
            theta = math.nan
            if env is not None:
                for e in env:
                    if not math.isnan(e) and (math.isnan(theta) or e > theta):
                        theta = e
                if not math.isnan(theta):
                    theta *= 2.0
            self.cost = [vec(cands[i]['total'], N) for i in rest]
            self.eligible = [math.isnan(theta) or any(c <= theta for c in cs) for cs in self.cost]
            byrank = sorted(range(n), key=lambda j: rk[j])
            self.rank = [0] * n
            for r, j in enumerate(byrank):
                self.rank[j] = r
            if not any(self.eligible) and n > 0:
                self.eligible[byrank[0]] = True
            self.center = [False] * n
            self.min_dist = [math.inf] * n
            self.ncenters = 0
        else:
            raise ValueError(did)

    def _center(self):
        best = -1
        for i in range(len(self.rest)):
            if not self.eligible[i] or self.center[i]:
                continue
            if best < 0 or (self.ncenters > 0 and self.min_dist[i] > self.min_dist[best]) or \
                    ((self.ncenters == 0 or self.min_dist[i] == self.min_dist[best]) and
                     self.rank[i] < self.rank[best]):
                best = i
        if best < 0:
            return -1
        self.center[best] = True
        self.ncenters += 1
        for i in range(len(self.rest)):
            if not self.eligible[i] or self.center[i]:
                continue
            d = js_distance(self.cost[i], self.cost[best])
            if d < self.min_dist[i]:
                self.min_dist[i] = d
        return best

    def pick(self):
        if self.did == 13:
            while True:
                i = self._center()
                if i < 0:
                    return -1
                if not self.taken[i]:
                    return i
        while self.next < len(self.order):
            i = self.order[self.next]
            self.next += 1
            if not self.taken[i]:
                return i
        return -1


def rest_key(p, N=20):
    """cmp_rest, but the position (appended by the caller's order)"""
    return (mean(vec(p['total'], N)), p['total_point'], p['startup_point'])


def diversify(did, cands, rest, k, env, cfg, ops=None, draws=None, N=20):
    """robustselect.c diversify(): the indices into cands (rest, in position
    order, those the main objective did not keep) the function picks, in
    order.  draws: Rand's, per rest entry (from the dump)"""
    taken = [False] * len(rest)
    members = [_Member(m, cands, rest, taken, env, cfg, ops, draws, N)
               for m in (MIX if did == 14 else (did,))]
    picks = []
    while len(picks) < k:
        progress = False
        for m in members:
            if len(picks) >= k:
                break
            j = m.pick()
            if j < 0:
                continue
            taken[j] = True
            picks.append(rest[j])
            progress = True
        if not progress:
            break
    return picks
