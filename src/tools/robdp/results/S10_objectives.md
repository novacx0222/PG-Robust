# S10 results (2026-11-20): the paper's objectives and diversification functions

## What was added

| Where | What |
|---|---|
| `util/robustselect.c` | the objectives as a loss per scenario (`loss_at`: Penalty, the penalty indicator, total, startup, point) and its aggregation over the scenarios (`aggregate`: max, mean, mean or max + λ·Stdev, α-quantile); the diversification functions (`diversify`: Rand, Similarity, Mix, E[Penalty]-Join, E[Cost]-Join), which pick the retained paths; `robust_select_final` scores the final candidates with the same two functions; dump fields `objective`, `strategy`, `ops` in `keep` |
| `guc_tables.c`, `robust.h` | `main_objective_id` takes the objectives, `retain_strategy_id` the objectives and the diversification functions (`check_robust_strategy`); `robust_penalty_lambda` (1.0), `robust_quantile_alpha` (0.85) |
| `robustsearch.c` | the envelope rounds run when the main objective or the retain strategy compares to C\* (Penalty-based objectives, Similarity, Mix, E[Penalty]-Join) |
| `tools/robdp/objectives.py` | the same in Python, shared by check_s8, check_s9 and check_s10 |
| `tools/robdp/check_s10.py` | gates O1–O8 |

Before S10, an audit of S9 found and fixed two more costs PostgreSQL changes after costing (see "Audit" below).

## Objectives (v20's numbering)

| Id | Paper | Loss x(s) | Aggregation |
|---|---|---|---|
| 0 | (v20) | Penalty(s) | max (from 0) |
| 1 | E[Penalty] | Penalty(s) | mean |
| 2 | (v20) | total(s) | max |
| 3 | E[Cost] (ranks as E[Gap]) | total(s) | mean |
| 4, 5 | (v20) | startup(s) | max, mean |
| 6 | ProbNoPenalty (as 1 − P) | 1 if Penalty(s) > 0 | mean |
| 8 | PostgreSQL | — | the point estimate of the total cost |
| 10 | (v20) | Penalty(s) | max + λ·Stdev |
| 11 | E[Penalty] + λ·Stdev | Penalty(s) | mean + λ·Stdev |
| 12 | Quant_α[Penalty] | Penalty(s) | ⌈αN⌉-th smallest |
| 17 | Quant_α[Cost] | total(s) | ⌈αN⌉-th smallest |
| 18 | (startup) | startup(s) | ⌈αN⌉-th smallest |

Penalty(s) = total(s) − C\*(s) if total(s) > (1 + τ) C\*(s), else 0 (τ = `robust_penalty_tau`). Stdev is the population's over the N scenarios. At the final relation the loss is taken of F(s), the cost of the fraction of rows fetched, against the cheapest F(s) of the candidates (S9).

## Diversification functions (retain strategy only)

| Id | Paper | What it picks from the candidates the main objective did not keep |
|---|---|---|
| 7 | Rand | uniformly: a draw per candidate from `pg_prng` seeded by `error_sample_seed`, the relation and partial / complete; the smallest draws |
| 13 | Similarity | farthest-first k-center by the Jensen-Shannon distance of the total cost vectors (normalized to sum 1, 1e-10 added), from the least E[Cost], among the candidates not above θ = 2·max_s C\*(s) in every scenario (if none is left, the least E[Cost]) |
| 14 | Mix | E[Cost], E[Penalty], E[Penalty] + λ·Stdev, Quant_α[Penalty], ProbNoPenalty, Similarity and E[Cost]-Join in turn, each its next pick not taken yet: the budget shared evenly |
| 15, 16 | E[Penalty]-Join, E[Cost]-Join | the candidates grouped by their last operator (`pathtype`: the join method, or the scan), the groups in the order of their best; round by round each group's next best |

Ties go to the lower E[Cost], then the point total and startup cost, then the position (as before).

## v20's bugs fixed

| v20 (`path/pathstrategy.c`) | S10 |
|---|---|
| τ fixed at 0.2 (`PENALTY_TOLERANCE_FACTOR 1.2`, line 8) | `robust_penalty_tau` (S8) |
| 10/11: λ fixed at 1; the Stdev over the first `effective` samples (line 291): 0 for a path whose cost is constant, though its Penalty varies with C\*(s) | λ a GUC; Stdev of Penalty(s) over all N scenarios |
| 12: a hard-coded 4th worst | Quant_α, α a GUC (0.85 = v20's 4th worst at N = 20, O4) |
| 6: "robust coverage", a greedy set cover with a fixed ε = 0.2 (line 702), higher scores better | the paper's ProbNoPenalty as a score, ranked like the others, τ from the GUC |
| 7: one xorshift state for the backend (line 845): a plan depends on what was planned before in the session | a stream per relation, seeded by `error_sample_seed` (O5) |
| 13: first center the least point cost; as many centers as `retain_path_limit` whatever the budget; a center could be picked twice; the ranking only partly filled | first center the least E[Cost]; distinct centers; the budget of the retain strategy; θ pruning as in the paper |
| 14: six strategies (worst penalty in place of the quantile, no E[Cost]-Join); its coverage member took the first unpicked candidate in list order | the paper's seven |
| a diversification function accepted as the main objective | rejected (O1) |

## Audit of S9: costs changed after costing

| Change | Where PostgreSQL makes it | Fix |
|---|---|---|
| a new target put into a path in place | `apply_projection_to_path` (e.g. the GatherMerge of a parallel ORDER BY over a volatile or SRF target, u25 / u26) | `robust_cost_project`: startup += Δs, total += Δs + Δpt·rows(s); `project` dump event; U4 recomputes it |
| the sort bound of the ordered relation | `create_ordered_paths` given the LIMIT bound | structural (the recorded bound for a Sort of the ordered relation), no longer a trial variant |
| the number of groups of a finalize Agg | `create_ordinary_grouping_paths`, from the grouped relation's rows | `est_input` tried first for a finalize Agg (177 such paths in U3) |

With these, check_s9 U1–U8 pass again (U4: 684 final picks, 5 targets put in place).

## Tests (`check_s10.py`)

Lognormal profile, N = 20, τ 0.2, add_path_limit 1, final limit 1; benchmark without q12_clique8, extra, forced parallel, S8 and S9 queries (serial and parallel).

| Gate | Result (release) |
|---|---|
| O1 settings | 62 tried: the main objective takes 0–6, 8, 10–12, 17, 18; the retain strategy also 7, 13–16; 9, 19, −1 rejected; λ ∈ [0, 1000], α ∈ [0.01, 1]; with `robust_envelope` off, exactly the Penalty-based objectives and functions are an error |
| O2 everything recomputed, 14 settings | 13,398 selections of 1,989,722 candidates, 1,904 final picks (1,806 with vectors), 3,081 scan/join scorings, exact; 10,899 paths retained by diversification functions, their pick order recomputed; no `env` event where nothing needs the envelope |
| O3 the gates see a difference | the same dumps recomputed with λ 0.75 for 0.5, α 0.9 for 0.95, λ 1 for 2, τ 0.25 / 0.3 for 0.2: 961, 775, 1,008, 589, 106 failures |
| O4 v20 parity | 48,138 scores of objective 12 (α 0.85) equal to v20's 4th worst penalty |
| O5 Rand | 118 queries, 957 selections: the same draws and picks in the second planning of the session and in a second run; 474 distinct draw sequences, none repeated within a planning |
| O7 check_s9 (→ S8 … S1) | U1–U8 pass |
| O8 planning time / stock (geometric mean) | B0 10.2; objectives 6, 10, 12, 17: 11.2–12.2; Rand 11.3; Similarity (K 1+3) 12.0; Penalty-Join 12.2; Mix (K 1+7) 16.4, max 605 (q11_clique6); without envelope (8, 18, E[Cost]-Join): 4.8–5.6 |

The debug build (cassert) passes O1–O5 and O8 (and check_s9 U1–U6, U8) after one fix (below).

**A fused multiply-add.** The debug build gave objectives 10, 11 one ulp off Python (1,105 scores): clang at -O0 fused `ss += d * d` into one rounding, -O2 did not. Every product added in the same expression in the code the gates recompute (variance, the Jensen-Shannon terms, F(s), the `project` shift) is now its own statement, as S2's statistics already were.

`make check` with robust planning on (lognormal; failing tests are all plan-only diffs, a subset of S9's lognormal set; no crash, no assertion failure, no new ERROR in the diffs):

| Setting | release | debug |
|---|---|---|
| main 0, retain 14 Mix, K 1+7 | 197/215 (107,191 paths retained) | 197/215 |
| main 0, retain 13 Similarity, K 1+3 | 197/215 | 197/215 |
| main 0, retain 7 Rand, K 1+2 | 197/215 | 197/215 |
| main 12 Quant, retain 11, K 1+2 | 199/215 | |
| main 6 ProbNoPenalty, retain 15, K 1+3 | 199/215 | |
| main 8 point, retain 16, K 1+2 | 201/215 | |

## What the settings do (O6, informational)

The final plan's F(s) against best(s), the cheapest final candidate of any setting in scenario s, 24 serial benchmark queries, geometric means. These are the planner's own cost vectors, on the same 20 scenarios the plans were chosen with.

| Setting | E[F] / E[best] | 1 + max Penalty / max best | mean F/best | worst F/best | plans ≠ B0 |
|---|---|---|---|---|---|
| B0 max Penalty | 1.294 | 1.339 | 1.832 | 3.012 | — |
| B3 E[Cost] | **1.185** | 1.471 | 1.618 | 2.835 | 30 |
| B8 point estimate | 1.373 | 1.949 | **1.340** | **2.229** | 61 |
| O6 ProbNoPenalty | 1.468 | 2.052 | 1.441 | 2.795 | 35 |
| O10 max Penalty + 0.5 Stdev | 1.319 | 1.345 | 1.877 | 3.058 | 8 |
| O12 Quant_0.95[Penalty] | 1.269 | 1.515 | 1.697 | 2.866 | 21 |
| O17 Quant_0.85[Cost] | 1.262 | 1.661 | 1.461 | 2.604 | 31 |
| O18 Quant[startup] | 192 | 323 | 271 | 1008 | 104 |
| R7 max Penalty + Rand | 1.294 | 1.339 | 1.832 | 3.012 | 0 |
| R13 + Similarity | 1.293 | 1.339 | 1.831 | 3.004 | 3 |
| R14 + Mix | 1.297 | **1.337** | 1.832 | 3.027 | 8 |
| R15 E[Penalty]+2 Stdev + Penalty-Join | 1.248 | 1.354 | 1.746 | 2.885 | 17 |
| R16 E[Cost] + Cost-Join | 1.185 | 1.471 | 1.618 | 2.835 | 30 |

**1. Each objective wins on its own measure.** The worst Penalty is lowest under max Penalty (B0 1.339, B8 1.949), the expected cost under E[Cost] (B3 1.185). The point estimate wins on the per-scenario ratios: Penalty is absolute, so max and E[Penalty] are decided by the expensive scenarios. In q11_clique6 B0's plan costs 375× the cheapest in a scenario where everything is cheap, and its worst Penalty is 5× lower than the point plan's (1.66·10¹⁷ vs 8.85·10¹⁷).

**2. The diversification functions change little here.** With these synthetic lognormal profiles, the retained paths rarely lead to a better final plan by the main objective: Rand changes no plan, Similarity 3, Mix 8, and the worst Penalty moves from 1.339 to 1.337 at best. Whether they help on errors the plans were not chosen with, or with real error profiles, this measure cannot show (see "Remaining").

**3. Startup objectives are for LIMIT plans.** Quant[startup] as the main objective of every join relation picks plans 190× the expected best; it is meant for the fractional final pick.

## Remaining

| What | Why it matters |
|---|---|
| Out-of-sample evaluation: score the chosen plans on scenarios not used to choose them (another `error_sample_seed`, or the real JOB profiles), and on execution | O6 measures the plans on the N = 20 scenarios they were chosen with, in the cost model; the paper's question is how they do on errors they did not see |
| The paper's Obj vs Obj_local, and an ensemble {⟨DV_i, K_i⟩} with its own budgets | one GUC drives both the local top-K and the final pick; Mix shares its budget evenly over a fixed set |
| Planning time with a large retain budget | Mix with K = 1 + 7 (the paper's K = 8): 16.4× stock, 605× on q11_clique6, as the retained pairing joins every kept path of both inputs |
| Penalty is absolute | the expensive scenarios decide max / E[Penalty]; a plan can be 375× the best in a cheap scenario (q11_clique6) and still be the best by the objective; a relative penalty would be a new objective, not a fix |
| GroupingSets, RecursiveUnion; subquery RTE rows; SubPlan costs in expressions | constant vectors there (S9) |
| EXPLAIN | the scores and vectors are only in the debug dump |
