# S8 results (2026-11-20): selection by the objectives

## What was added

| Where | What |
|---|---|
| `optimizer/robustselect.h`, `util/robustselect.c` (new) | objectives 0–5, candidate collection with a twin filter, the selection (main objective, retain strategy, unparameterized rescue), `keep` dump events, the score compare |
| `pathnode.c` | `add_path` / `add_partial_path` collect every candidate in the main pass (unless `robust_dominance`); `compare_path_costs` compares two scored paths by score first |
| `robustsearch.c` | `select_rel`: partial paths, then Gather on the kept partial paths, then complete paths, then `set_cheapest`; child join rels of partitionwise joins selected too; partial envelope C\*_p; level-1 rels scored; rounds only when an objective needs the envelope |
| `allpaths.c` | the round and main loops call `robust_search_finish_rel`; `generate_partitionwise_join_paths` hands child rels to `robust_search_child_rel` |
| `joinpath.c` | `robust_join_pairing = retained`: the join builders also try the inputs' other unparameterized kept paths and all their kept partial paths |
| `prepunion.c` | `choose_hashed_setop`'s two stack Paths get `robust = NULL` (the score compare reads it; the debug build crashed on it) |
| GUCs | `robust_select` (on), `robust_dominance` (off), `robust_join_pairing` (retained / native), `add_path_limit` (1), `retain_path_limit` (1), `robust_final_path_limit` (1), `main_objective_id` (0), `retain_strategy_id` (0), `robust_penalty_tau` (0.2) |

## Selection (v20's global top-K, its bugs fixed)

After each join rel is complete, from all its candidates:

1. the `add_path_limit` best by the main objective;
2. of the others, the `retain_path_limit` best by the retain strategy;
3. if no kept complete path is unparameterized, the best unparameterized candidate (P2);
4. at the final rel: `robust_final_path_limit` by the main objective (1, as in v20), no retained path.

Ties: score, then expected total, then point total, point startup (as `compare_path_costs` falls back), then position. The kept paths are the new pathlist in that order. They keep their scores, so `set_cheapest` and `get_cheapest_path_for_pathkeys` pick by the objective.

| Id | Objective | Needs the envelope |
|---|---|---|
| 0 | worst penalty max_s Penalty(s) | yes |
| 1 | expected penalty mean_s Penalty(s) | yes |
| 2 | worst total cost | no |
| 3 | expected total cost | no |
| 4 | worst startup cost | no |
| 5 | expected startup cost | no |

Penalty(s) = total(s) − C\*(s) if total(s) > (1 + τ) C\*(s), else 0.

**Two envelopes.** Complete paths against C\*(U, s): the round's cheapest total, Gather paths included. Partial paths against C\*_p(U, s): the round's cheapest partial path (per worker). In v20 the partial paths were compared with the complete envelope, so their penalty was always 0.

**Order within a rel.** Partial paths are selected first, then Gather paths are built on the kept ones, then the complete paths (Gather included) are selected. In v20 the Gather paths came after the selection and were never pruned (N8).

| v20 issue | S8 |
|---|---|
| P2: all kept paths parameterized | rescue of the best unparameterized path (`by = unparam`) |
| N8: Gather paths not selected | built on kept partial paths, then selected |
| N9: partitionwise child joins not selected | selected against their own envelope (`env.child`) |
| partial paths vs the complete envelope | their own envelope C\*_p |
| O5: ties by candidate position | the order above, consistent with `set_cheapest` |
| τ fixed; objectives 8/9 not registered | `robust_penalty_tau`; ids above 5 rejected |
| always the envelope pass | rounds only for objectives 0/1; an error if they need it and `robust_envelope` is off |

Kept on purpose (v20 behaviour): one ranking for all paths (no grouping by pathkeys or parameterization); the final rel keeps 1 path.

## Tests (`check_s8.py`)

| Gate | Result (release) |
|---|---|
| G1 trivial / all-ones, objective 3, no retained, `robust_dominance` on, limits 10^6, native pairing: plans identical to stock | 136 plannings (benchmark, extra, forced parallel, S8 queries with partitionwise joins), 3,132 selections keeping all their candidates. With the final limit 1 (v20): q41_order_limit and f4_order differ (a sorted path of the final rel wins in stock) |
| G2 every `keep` event recomputed in Python (scores, kept, why), exactly | 5,535 selections of 964,723 candidates, 5 settings (objectives 0–5 as main or retain, limits 1–3 + 0–2, final 1–2, τ 0.2 / 0.5, retained / native), 15,615 paths kept, all exact |
| G3 pathlist = kept paths in order; `cheapest_total` = first unparameterized kept; unparameterized path always kept | 2,818 pathlists; 11 selections needed the unparameterized rescue (f5, f6) |
| G4 Gather candidates on kept partial paths (N8); child join rels selected against their envelope (N9) | 1,266 Gather candidates; 120 child join rel selections, 64 child env events |
| G5 settings | objective 6 rejected; objective 0 with `robust_envelope` off is an error; objectives 2/5 run no round; level-1 rels scored, not pruned |
| G6 base rels: kept = C\* (complete and partial) | 5,560 (U, s), all equal |
| G7 check_s7 with `robust_select` off (and through it check_s6 .. s1) | K1–K8 pass |
| G8 planning time / stock, geometric mean | S7 15.2 (max 829 q12_clique8); S8 12.3 (max 68); native pairing 10.9 (max 52); objectives 3/2, no envelope 4.8 (max 27); trivial profile 7.4 (max 47) |

The debug build (cassert, `--light`: q12_clique8 left out of the lognormal runs) passes G1–G8: 4,005 selections of 540,741 candidates recomputed exactly, 11 rescues, 774 Gather candidates, the same 120 child join rel selections; G7 (check_s7 on debug) passes K1–K8.

| `make check` | debug | release |
|---|---|---|
| plain | 215/215 | 215/215 |
| robust on, trivial, identity settings of G1 | 214/215 (sysviews), 29,873 selections | 214/215 (sysviews) |
| robust on, trivial, S8 defaults | 202/215 | 202/215 |
| robust on, lognormal, S8 defaults | 197/215 | 197/215 |

With the S8 defaults the failures are all plan-dependent output: plan shapes (join, partition_join, partition_aggregate, select_parallel, incremental_sort, ...), row order without ORDER BY (merge), the multibatch flag (join_hash), sysviews. No error, no crash, no assertion failure. The two envelope mismatches of S7 remain (lognormal, partition_join).

## Findings

**1. Retained vs native pairing changes little with 1 + 1 paths per rel.** Objective 0, lognormal, 62 final rels: worst penalty equal in 61, higher with retained in 1 (q11_clique6, by 4·10⁻⁸ relative: same plan shape, costs ~2.5·10¹⁷); expected total lower with retained in 2. Retained offers 2.2× the candidates (366k vs 164k) but the inputs keep only 2–3 paths, so the extra pairings rarely win.

**2. The kept paths are far from the envelope of each scenario.**

| Join rels, lognormal, objective 0 | kept = C\* | > 1.2 C\* | > 2 C\* | geo-mean kept/C\* |
|---|---|---|---|---|
| S7 (dominance, ~16× the paths) | 61.7% | – | 6.5% | – |
| S8 retained, complete | 19.1% | 65.4% | 49.7% | 3.99 |
| S8 retained, partial (C\*_p) | 22.8% | 60.8% | 44.5% | 3.16 |
| S8 native, complete | 19.4% | 64.8% | 49.2% | 3.93 |

Expected: one path (plus one retained) can't be cheapest in 20 scenarios. The worst penalty objective picks the path whose worst scenario is least bad, not one near C\* in each.

**3. Parameterized join paths are rare.** PostgreSQL builds them for inner joins only when the join order is restricted (or a star schema), so P2 never shows in the benchmark. f5/f6 (an outer join over an inner join) give the inner join rel 16–24 paths parameterized by t1, which every objective ranks first; the rescue keeps an unparameterized one.

**4. Planning time** (geometric mean / stock, release): S7 15.2 (clique8 829×) → S8 12.3 (clique8 68×). Without the envelope (objectives 2–5) 4.8 (clique8 27×). Debug: 21.5 → 17.3. The 20 envelope rounds are now most of the time.
