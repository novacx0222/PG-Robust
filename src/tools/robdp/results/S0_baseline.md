# S0 results (2026-11-18)

## Baselines

| Check | Result |
|---|---|
| `make check`, v2-debug and v2-release (pure REL_16_2) | 215/215 |
| Statistics fingerprint across two full reloads | identical (`6860b786…`) |
| Stock plans, run twice (`stock_v2rel.json` vs `stock_v2rel_b.json`) | 25/25 SAME |
| Old p0 with `enable_rows_dist=off` vs stock | 25/25 SAME |

## Reference: old v20 (p0-release) with trivial profiles (x ≡ 0)

Settings:

- `trivial` profiles (sel_true = sel_est) and `ones` profiles ("1 1").
- 20 samples, 2 bins, bandwidth 0.
- `add_path_limit=1`, `retain_path_limit=0`.
- Objective 3 (E[Cost]) and objective 1 (E[Penalty]).

All four combinations gave the same result: 20/25 queries are SAME as stock, with identical plan, rows and costs. The other 5 are PLAN-DIFF:

| Query | v20 plan total vs stock | Why |
|---|---|---|
| q10_clique4 | −0.147% | Within PostgreSQL's 1% `add_path` fuzz. Stock keeps a fuzzily-equal path with a better startup cost or useful pathkeys; v20 ranks only by total cost. Not an error. |
| q11_clique6 | −0.272% | Same reason. |
| q12_clique8 | −0.272% | Same reason. |
| q34_in_subquery | −0.869% | Same reason. |
| q41_order_limit | **+41589%** | Real regression. v20 keeps only the top-1 path per joinrel by total cost. That drops the cheap-startup NL path sorted on `t1.id`, which `ORDER BY … LIMIT 10` needs. Stock costs 4.72; v20 costs 1967.72. |

Conclusions for v2:

- Where the plans agree, the v20 cardinality and cost vectors reduce exactly to stock under a trivial profile.
- The v20 problem is in pruning: it ignores pathkeys and startup cost.
- The v2 gate after S8 is: trivial profile with E[Cost] gives 25/25 SAME. This requires the vector-dominance `add_path` to reduce to the stock fuzzy `add_path` when every vector is constant.

Planning time (ms, stock → v20 E[Cost] → v20 E[Penalty]):

| Query | Stock | v20 E[Cost] | v20 E[Penalty] |
|---|---|---|---|
| q07_chain8 | 0.29 | 0.81 | 3.35 |
| q12_clique8 | 5.08 | 23.4 | 141.6 |
| q20_star | 0.32 | 1.01 | 4.50 |
