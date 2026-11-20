# S9 results (2026-11-20): above the join search

## What was added

| Where | What |
|---|---|
| `util/robustcost.c` | replay of the upper paths: Agg, Group, UpperUnique, WindowAgg, SetOp, LockRows, ModifyTable, Limit, and the Gather / GatherMerge of a partial aggregation; the number of groups per scenario (`RobustGroups`); a side table (`UpperInfo`) for what a constructor is given and the path does not keep; dump fields `upper`, `groups` |
| `pathnode.c` | `create_agg_path`, `create_windowagg_path`, `create_limit_path` record their AggClauseCosts, window functions, offset/count estimates; `add_path` / `add_partial_path` compute an old path's vectors lazily (paths projected in place) |
| `util/robustselect.c` | `robust_select_score_rel` (the scan/join relation scored, not pruned); `robust_select_final` (the final pick of each query level by the main objective of F(s)); `final` dump event |
| `planner.c` | `get_cheapest_fractional_path` = PostgreSQL's choice, then `robust_select_final`; `create_ordered_paths`' sort bound recorded; `robust_select_score_rel` before the last `set_cheapest` of `apply_scanjoin_target_to_paths`; `robust_cost_set_root` before `plan_set_operations` (set-operation query levels) |
| `subselect.c` | `SS_charge_for_initplans` → `robust_cost_charge`: the cost of a query level's initPlans, added to its final relation's scalars after the paths are costed, is added to their vectors too; `charge` dump event |
| `common.py` | the bench server runs with `log_min_messages=fatal`, so the dump lines stay out of server.log |

## Design (the user's choices D1–D3)

**Upper replay.** An upper path is costed as its `create_*_path` function costs it, with the rows and costs of its input from the input's vectors. What the path does not keep is recorded by the constructor (`UpperInfo`, keyed by the Path): an Agg's AggClauseCosts, a WindowAgg's window functions, a Limit's `offset_est` / `count_est`. As in S6, the point replay must reproduce the scalars bit for bit.

**Number of groups (D3).** PostgreSQL computes it before creating the path and does not record how. The replay tries, in order, and keeps the first that reproduces the point:

| `groups` | number of groups in s |
|---|---|
| `est_sub` | `estimate_num_groups(group exprs, rows of the input path in s)`, base relation rows in s |
| `est_input` | same, with the rows of the relation the grouping reads (the scan/join relation, also for a partial aggregation's finalize step) |
| `sub_rows` / `input_rows` | those rows themselves (set operations' UNION dedup, DISTINCT on unique keys) |
| `frozen` | the point's in every scenario (SetOp; expressions with Vars of no base relation) |

**Sorts of the ordered relation.** `create_ordered_paths` is given the LIMIT's bound even where `root->limit_tuples` is −1 (GROUP BY, aggregates or window functions below the ORDER BY); `grouping_planner` records it (`robust_cost_ordered_limit`) and a Sort replay variant uses it (found by U3 on u24_initplan_limit).

**Gather of a partial aggregation.** `gather_grouping_paths` passes rows = input rows × workers; a replay variant tries this when the plain one does not reproduce the point.

**Scan/join relation.** Once the final target is applied and the top Gather paths are built, every path of the relation is scored by the main objective against the cheapest of them in each scenario (complete against the unparameterized ones, partial against partial), without pruning. That covers the top Gather paths and the only relation of a query without joins, which S8 never selected.

**Upper relations** keep the paths no other dominates in every scenario (S7's vector `add_path`).

**InitPlans.** PostgreSQL adds the cost of a query level's initPlans (uncorrelated scalar subqueries, CTEs) to the paths of its final relation once they are costed. The vectors get the same constant (the initPlans' costs are their plans', constant across scenarios); before this fix a candidate with vectors was compared without it against a constant candidate with it, and a FROM subquery's SubqueryScan read input vectors without it. Found by U4: on u22_cte_limit the final scores held a cost (the CTE's) the path events did not.

**Final pick (D1, D2).** `get_cheapest_fractional_path`, for each query level (top level, set-operation leaves, subplans), picks among the final relation's paths:

F(s) = startup(s) + f · (total(s) − startup(s)), f = `tuple_fraction`, divided by rows(s) if ≥ 1; F(s) = total(s) if f ∉ (0, 1).

Objectives 0/1: Penalty against min over the candidates of F(s); 2/3: F(s); 4/5: startup(s). Ties: score, mean F, F at the point, point startup, position. Without vectors PostgreSQL's choice stands. `robust_final_path_limit` stays 1 (D2); K = 4 is tested in U6.

**Not covered.** GroupingSets and RecursiveUnion are not replayed (status `unsupported`, constant vectors); MinMaxAgg is costed from its subqueries' plans and is constant. A subquery RTE (set-operation leaves, FROM subqueries) takes its rows from the subquery's point estimate, not its vectors (the rows model of S4).

## Tests (`check_s9.py`)

| Gate | Result (release) |
|---|---|
| U1 trivial / all-ones, S8's identity settings: plans identical to stock; upper paths at their point; no final pick with vectors | 232 plannings (benchmark, extra, forced parallel, S8 and S9 queries), 876 upper paths, 268 final picks, all PostgreSQL's |
| U2 upper replay vs stock PostgreSQL on the scaled query (one-point profile x = ln K) | 45 cases, 220 upper paths equal within 1e-6, all off their point: Agg 88, Sort 48, UpperUnique 24, Limit 18, IncrementalSort 9, Gather 9, GatherMerge 6, Projection 6, WindowAgg 6, LockRows 3, ModifyTable 3 |
| U3 lognormal, 5 settings: no upper path `mismatch` / `nocontext` | 2,054 replayed, 70 constant, 40 unsupported (GroupingSets, RecursiveUnion); groups from `est_sub` 1,170, `sub_rows` 15, `frozen` 10 (`est_input` never needed) |
| U4 every final pick recomputed in Python (F(s), envelope, scores, pick; `charge` applied) | 664 picks of 1,058 candidates, exact; 629 with vectors, 20 fractional, 30 final relations charged for initPlans; 57 differ from PostgreSQL's |
| U5 every scan/join scoring recomputed (own envelope, all kept, order) | 1,077 scorings, exact; 366 Gather candidates, 376 single relations |
| U6 final relation keeping 4 paths instead of 1 | changes 14 plans (2 with LIMIT) |
| U7 check_s8 (through it S7 .. S1) | G1–G8 pass |
| U8 planning time / stock, geometric mean | bench 13.3 (max 79 q12_clique8; S8 12.3, max 68); S9 queries 4.9; K = 4: 13.8; objectives 2–5, no envelope: 5.2; trivial profile 8.0 |

The debug build (cassert, `--light`, without U7) passes U1–U6 and U8 with the same counts within a few plannings (q12_clique8 left out of two settings).

Final picks per setting (release, lognormal, 20 scenarios):

| Setting | Picks with vectors | Differ from PostgreSQL |
|---|---|---|
| A objective 0, final K 1 | 127 | 6 |
| K4 objective 0, final K 4 | 127 | 18 |
| C objective 2, final K 2 | 125 | 10 |
| D objective 4, final K 1 | 125 | 10 |
| B objective 1, τ 0.5, final K 3 | 125 | 13 |

| `make check` | debug | release |
|---|---|---|
| plain | 215/215 | 215/215 |
| robust on, trivial, identity settings | 214/215 (sysviews) | 214/215 (sysviews) |
| robust on, trivial, S8/S9 defaults | 202/215 | 202/215 |
| robust on, lognormal, S8/S9 defaults | 197/215, 1,708 final picks with vectors, 334 differ | 197/215, 1,709, 335 |

The failing tests are exactly S8's, with plan-only diffs (sizes equal to S8's but foreign_key, memoize, partition_join in lognormal). No error, no crash, no assertion failure; 0 upper mismatches; the 2 envelope mismatches of S7 remain; RecursiveUnion is the only upper type unsupported there (11 paths).

## Findings

**1. Where the final pick differs (objective 0).** With K = 1 (setting A) the scan/join relation of a join query keeps 1–2 paths, so the alternatives come from the upper relations (grouping strategies, partial or not; DISTINCT; LIMIT over differently sorted inputs) or from a query without joins, whose only relation keeps all its paths (u13, u19). In u02_having (parallel) the worst penalty drops from 37,280 to 1,602 at a point cost 41% higher; u03_distinct goes from 346 to 0. Where both have penalty 0, the lower expected F decides (u13: a BitmapHeapScan, mean F 13.96 vs 14.94).

**2. K = 4 at the final relation** brings the join-tree alternatives to the pick (U6: 14 plans change). The worst penalty falls by 49–59% on the cliques and 78% on q62_union_all, and to 0 in u14_limit_join3 (from 13,000). Penalty is absolute, so the most expensive scenarios dominate it: in q10_clique4 the pick halves the worst penalty (7.8·10¹⁰ → 4.0·10¹⁰) while its worst F/F\* ratio rises from 15× to 288× in a cheaper scenario.

**3. Two in-place cost changes PostgreSQL makes after costing** were invisible to replay and are now covered: the initPlans' cost added to the final relation's paths (vectors were short by it), and the LIMIT bound of the ordered relation's sorts.

**4. Planning time** rises little (12.3 → 13.3 geometric mean): the upper relations are small; the 20 envelope rounds of the join search remain most of the time.
