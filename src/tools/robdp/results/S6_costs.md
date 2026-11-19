# S6 results (2026-11-18): cost vectors of paths ("replay")

## What was added

| Where | What |
|---|---|
| `optimizer/robustcost.h`, `util/robustcost.c` (new) | `RobustPathCost` (`Path.robust`): `status`, `rows`/`startup`/`total` vectors (NULL = the scalar in every scenario), HashPath `nbatches` per scenario |
| `add_path`, `add_partial_path` | every offered path is costed per scenario; its inputs first (ids in dump order) |
| `add_paths_to_joinrel` | the join context (join type, sjinfo, restrictlist, semi factors) the replay of a join path needs |
| `standard_planner` | per-planning state, restored on error (`PG_TRY`/`PG_FINALLY`); `subquery_planner` / `query_planner` keep the current query level |
| `costsize.c`, `selfuncs.c` | hooks, active only during a replay (`robust_replay != NULL`): index selectivity (btree and generic), `approx_tuple_count`, semi/anti factors, hash bucket stats, Memoize `estimate_num_groups` |
| `robustrows.c` | querylet cache per query level; querylets carry their base rels (`ia`, `ib`) |

The scalar fields stay PostgreSQL's. Nothing reads the vectors yet (S7).

## Replay

The cost of a path in scenario s is PostgreSQL's own cost function run on a bitwise copy of the path. Its inputs are set to their values in s and restored afterwards:

- the rows of every base rel, of the path's rel or ParamPathInfo;
- each input path's rows, startup and total cost (from its vectors), a hash join input's batches, a Memoize inner's subpath and calls (= outer rows[s]);
- the selectivities cost functions estimate by themselves, scaled by the multipliers of the querylets their clauses belong to (restriction clause → the rel's querylet, two-rel clause → the pair querylet, 3+-member equivalence class → the class factor).

Decisions that shape the plan stay at the point estimate's choice (merge join `materialize_inner`, Unique method). Decisions that do not are replayed (hash batches).

**Hash bucket size.** PostgreSQL caches a hash clause's bucket size in the RestrictInfo, estimated for the buckets of the first hash join costed with it (the "filler"). The replay redoes the estimate with the scenario's rows and the buckets the filler has in that scenario; the filler's own replay records them. Using the point's buckets instead was off by up to 6% on hash joins against stock (C4).

**Values PostgreSQL does not derive from the path.** The regression suite (robust on, dump on) turned up three, each a point-check mismatch until handled:

| Case | What PostgreSQL does | Replay |
|---|---|---|
| Partitionwise nested loop over a Memoize | `try_nestloop_path` costs the join while the Memoize's `param_exprs` are the parent's Vars; `reparameterize_path_by_child` then translates them to the child's (whose own statistics give another distinct count) | estimates with the Vars the join was costed with, recorded when the Memoize is translated |
| Parallel Append mixing partial and non-partial children | `add_paths_to_append_rel` gives it the rows of the first, all-partial Append (`partial_rows`), not its children's sum | rows[s] of that Append |
| Bitmap heap scan of a rel with lateral references | built with `loop_count = 1` for the rel's own lateral parameterization | point check tries `get_loop_count`, then 1 |

`ProjectSet` paths (set-returning functions in a join rel's target list) are replayed with `create_set_projection_path`'s formula.

**Point check.** Before the scenarios, a replay with nothing changed must reproduce the scalars bit for bit. Otherwise the path gets status `mismatch` and constant vectors. Semi factors that the replay does not reproduce are kept at the point (`semi_frozen`). Upper rels are skipped (S9).

**Speed.** Each scenario runs in a scratch memory context, reset afterwards. Querylets are cached per query level. Bucket-size and Memoize group estimates are cached under the inputs they depend on, with word-wise hashed keys.

## Tests (`check_s6.py`)

| Gate | Result (release) |
|---|---|
| C1 plans unchanged, dump on (every path replayed) | 75/75 identical to stock over trivial, two-point and lognormal profiles; 25/25 forced-parallel plannings (24 with Gather) identical to robust off |
| C2 trivial and all-ones profiles, benchmark + extra queries, serial + forced parallel | 299,416 paths replayed, 1,152 constant, none `mismatch` / `unsupported` / `nocontext`; rows, startup, total[s] == point bit for bit |
| C3 wiring, lognormal | 410,485 checks hold (149,494 paths with a varying total): scan/join rows = rel/ppi vector, Material and Memoize follow their input, Memoize adds `cpu_tuple_cost`, non-partial SeqScan constant, inputs dumped first, all finite and ≥ 0 |
| C4 one-point profiles scaling a filter by K ∈ {0.5, 2, 3} vs stock PostgreSQL on the query whose filter selects K× as much (13 templates: inner, left, full, semi/anti ×2, indexed filter, chain3 middle filter, param NL, IN-subquery Unique, partitioned, partitioned join) | 785 common paths equal to 1e-6 (443 with a cost different from the point). Partitioned cases to 1%, since each partition's MCV list makes its rows linear in the threshold only to ±0.06%. 36 paths of the partitioned join not compared: there the stock join rows themselves move (eqjoinsel caps p.id's distinct values at the filtered rows), which the S5 model holds at the point. |
| C5 S5 gates (S4…S1 through them) | pass |
| C6 planning time (trivial, N=20, dump off) | on/off geometric mean 2.57, max 7.97 (clique8); vs v20 E[Cost] geometric mean 1.47, max 2.00 |

The debug build (cassert) passes C1–C5 with the same counts. Its C6 ratio against v20 compares a debug build with v20's release numbers and means nothing.

| `make check` | Result |
|---|---|
| plain, debug + release | 215/215 |
| robust on with the dump on (`TEMP_CONFIG`: lognormal, N=20), debug + release | 214/215, only sysviews (`enable_rows_dist = on`). No assertion failure. Every path replayed or constant (release: 27,530 replayed, 260,227 constant); no `mismatch`, `unsupported` or `nocontext`. Before the three fixes and ProjectSet support (Replay section): 14 mismatches and 1 unsupported. |

## Planning time

v20 kept one path per rel in pass 2 (`add_path_limit = 1`) and costed only those per scenario. v2 replays every path offered to `add_path` (clique8: 40,358 paths × 20 scenarios, about 40 ns per path and scenario). Clique8 went from 54.6 ms to 37 ms with the caches (v20: 23.4 ms; stock 5.0 ms). The remaining time is PostgreSQL's cost functions themselves (`initial_cost_mergejoin`, `cost_qual_eval`, `approx_tuple_count`, `final_cost_hashjoin`, …); cache lookups are about 3%. The 1.2× v20 target of the plan is not met. Pruning before replay (S7/S8) is where it can be won back, since most offered paths are rejected by `add_path`.

**Decision (2026-11-18).** The gap is accepted for S6. The ratio against v20 compares different amounts of work: v20 kept one path per rel, while v2 gives every offered path its vectors. The geometric mean is also dominated by small queries, where the difference is a few microseconds of fixed per-planning work. The planning-time gate is to be restated against stock PostgreSQL once S7/S8 fix which paths need vectors.
