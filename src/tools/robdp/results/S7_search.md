# S7 results (2026-11-19): join search with the envelope and vector dominance

## What was added

| Where | What |
|---|---|
| `optimizer/robustsearch.h`, `util/robustsearch.c` (new) | envelope rounds: the round view of the existing rels, save/restore of the search state, C*(U, s), `env` / `envstat` / `pathlist` dump events |
| `allpaths.c` | `make_one_rel` sets the passes (`base` → `envelope` × N → `main` → `upper`); `standard_join_search` hands a `base` search to `robust_join_search` |
| `pathnode.c` | `add_path` / `add_partial_path` compare cost and rows vectors per scenario when either path has them (passes `base`, `main`, `upper`); the prechecks are off in `main` when some input varies |
| `robustcost.c` | envelope rounds replay round paths in the round's scenario only and write the result into their scalars; `st->varies`; save/restore of the replay state around a round |
| `robustrows.c` | rels and ParamPathInfos take `rows[s]` in round s |
| GUCs | `robust_search` (default on: S7 search; off: S6 behaviour), `robust_envelope` (default on; off skips the rounds, for K3 and K8) |

## Envelope pass

For each scenario s, one round of PostgreSQL's own join search (`join_search_one_level`, partitionwise joins, Gather) runs in its own memory context:

- **Round view.** Every rel that existed before the search is projected to s: rel and ppi rows, the scalars of its paths (and HashPath batches), `set_cheapest` again. Everything is restored after the round.
- **Join rels.** Join rels the round builds see `rows[s]`. Their paths are costed by PostgreSQL in the round view. A path whose cost depends on more than the view (selectivities, bucket sizes) is replayed at s after the point check. The result is written into its scalars, so `add_path` in the round is PostgreSQL's own with s's costs.
- **Search state.** Restored at each round start and before the main pass:
  - `join_rel_list` / `join_rel_hash` (as `geqo_eval` does);
  - the RestrictInfo selectivity and bucket caches (a snapshot);
  - the replay state: Append / Memoize records, bucket fillers, join context.
- **C\*(U, s).** The cheapest total cost of each rel U at the end of round s.

With a profile that does not vary (`st->varies` false), the rounds replay nothing and the main pass is exactly stock PostgreSQL.

## Main pass

The join search runs again at the point, with vector dominance in `add_path`. For each scenario s, the fuzzy compare of total and startup costs (fuzz 1%):

- equal in every s → equal;
- better or equal in every s, better in one → better;
- otherwise → different, and both paths are kept.

Rows count as better only if they are no larger in every scenario. Paths whose rows are not comparable are both kept.

## Tests (`check_s7.py`)

| Gate | Result (release) |
|---|---|
| K1 trivial / all-ones profiles: plans identical to stock; lognormal: dump on = dump off | 166 plannings identical |
| K2 trivial / all-ones: C\*(U, s) = stock cheapest total bit for bit, no round replay | 1,952 rels × 20 scenarios |
| K3 lognormal: main-pass events identical with `robust_envelope` on and off | 517,148 events |
| K4 one-point profiles scaling a filter by K: C\*(U, 0) = stock cheapest total of the query whose filter selects K× as much | 39 cases, 114 rels (48 differ from the unscaled query; 3 partitioned cases to 1%); 3 rels not compared (their rows differ from stock) |
| K5 envelope vs main pass, lognormal | base: 2,800 (U, s), kept ≥ C\* everywhere. Joins: 6,520 (U, s); kept = C\* 61.7%, below 3.5% (min 0.9876), above 34.8% (max 14.26) |
| K6 no kept path dominated by another kept path in every scenario | 18,791,890 pairs, none; 30,504 paths kept (lognormal) vs 1,874 (trivial = stock) |
| K7 check_s6 (robust_search off) | C1–C5 pass with the S6 counts |
| K8 planning time / stock, geometric mean | S6 2.48 (max 7.8 clique8); trivial 5.49 (max 33.1); lognormal 14.51 (max 907); lognormal without envelope 5.95 (max 858) |

The debug build (cassert) passes K1–K7 with the same counts; K7 also runs check_s6 there. Its K8 times are a debug build's and mean nothing: geometric mean 7.71 trivial, 21.36 lognormal.

| `make check` | Result |
|---|---|
| plain, debug + release | 215/215 |
| robust on, trivial profile, dump on, debug + release | 214/215 (sysviews); envstat (release): 2,731,180 const, no replay, mismatch, unsupported or nocontext; main pass 27,413 replayed, 260,360 const |
| robust on, lognormal, dump on, debug + release | 208/215: sysviews, plus plan-shape diffs in updatable_views, join, rowsecurity, partition_join, partition_prune and partition_aggregate (a different plan chosen; no error). No assertion failure. Main pass: no mismatch. Envelope: 364,289 replayed, 2,364,354 const, 2 mismatch |

**Residual: 2 envelope mismatches** (partition_join, `t1.a IN (SELECT … prt2 t1, prt1_e t2 …)`). The path is a parallel Append mixing non-partial and partial children of a child join rel, whose rows were not given by `add_paths_to_append_rel`. In the round, its rows come from its children's scalars when it was built. Its point check gives 2 rows against a scalar of 3. These two paths keep the scalars PostgreSQL gave them in the round view. That is 2 out of about 2.7 million round paths.

## Findings

**1. The main pass does not reach the envelope on join rels.** In 23.1% of (join rel, s), the best kept path costs more than 1.01 × C\*(U, s): 16.7% above 1.1×, 6.5% above 2×, at worst 14.26×. Cause: PostgreSQL builds join paths from a few inputs per side (`cheapest_total_path`, the cheapest per pathkeys, `cheapest_parameterized_paths`). The main pass keeps many more paths per rel, but the join builders still pick their inputs at the point. The round for s builds with the inputs cheapest *in s*, so the combination giving C\*(U, s) is often never generated in the main pass.

Example: q07_chain8, U = {t3…t7}, s = 5. Kept 16,757 vs C\* = 9,439. The round's cheapest is a HashJoin whose {t3…t6} input is that rel's cheapest path in s. In the main pass, no HashJoin of {t3…t7} has that input.

The 3.5% below C\* (min 0.9876) are presumably the converse, not investigated further. The round's `add_path` prunes within the 1% fuzz factor at a lower level. The main pass keeps that path (another scenario favours it), and it combines into something cheaper.

**2. Pathlist explosion.** Under lognormal, almost every pair of paths crosses in some scenario, so dominance prunes little:

- benchmark: 30,504 kept paths vs 1,874 stock;
- clique8: 64k vs 565;
- planning time of clique8: 907× stock.

The envelope adds 2.4× on the geometric mean (14.51 vs 5.95). The rest is the main pass's larger pathlists × the replay.

**Both point to S8.** Prune the main pass's pathlists with the envelope (top-K by regret against C\*), and give the join builders inputs that are cheapest in some scenario (per-scenario cheapest paths), not only at the point.
