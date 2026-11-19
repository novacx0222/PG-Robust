# RobDP v2 debug dump (contract between the planner and the test harness)

Steps S1–S9 implement this in the planner. The harness (`robdp_dump.py`) parses it. Every numeric check in the plan uses these lines, because EXPLAIN rounds costs to two decimals.

## Output

GUC `robust_debug_dump` (bool, `DEVELOPER_OPTIONS`, `PGC_SUSET`, default off).

When it is on, the planner emits one line per event:

```
elog(LOG, "ROBDP %s", json_object)
```

The client receives these lines with `SET client_min_messages = log`. psql prints them on stderr as `LOG:  ROBDP {...}`.

Formatting rules:

- Every double is printed with `%.17g`, so values round-trip exactly.
- JSON has no NaN or Infinity. Those values are written as the strings `"nan"`, `"inf"` and `"-inf"`.
- A relation is named by `"rels"`: a space-separated, sorted list of its base relations' names, for example `"t1 t2"`. Outer-join relids are left out. A base relation's name is its alias, plus `#<rti>` when another range-table entry of the same query level has the same alias (`t1#1`, `t1#3`). An inheritance child is named `<parent>:<relation>` (`p:p1`), because PostgreSQL gives children their parent's alias.

## Events

| `ev` | Emitted when | Fields |
|---|---|---|
| `query` | `standard_planner` starts (S1) | `enabled`, `N`, `seed`, `bins`, `bandwidth`, `profile_path` |
| `profile` | a profile is loaded or reloaded (S2) | `key`, `path`, `n` (valid points), `skipped` (invalid lines), `bins`: list of `{first, n, lo, hi, mean, sd, h}` |
| `draw` | a querylet's samples are drawn (S3) | `key` (profile key), `inst` (instance key), `seed` (16 hex digits), `sel_est`, `bin`, `h` (bandwidth used), `x` (list of N) |
| `rel` | a rel gets its row vector (S4/S5) | `pass`, `rels`, `kind` (`base`/`join`/`append`/`other`), `tuples`, `sel_est` (base/other only), `querylets` (list of `{key, inst, sel_est, bin}`, only those with a profile), `children` (append only: the live children's rels, in summation order), `rows_point`, `rows` (list of 1 or N), `pair` (join only: `[outer rels, inner rels]` of the pair that built it) |
| `rel` (kind `join`) | a join rel gets its vector from its first pair (`first: true`), and, with the dump on, every later pair reports the vector it would give (`first: false`, not stored) (S5) | `pass`, `rels`, `kind`, `first`, `jointype` (`inner`/`left`/`full`/`semi`/`anti`), `pair` (`[outer rels, inner rels]`), `sel` (`[fkselec, jselec, pselec]` from `calc_joinrel_size_estimate`), `sels` (semi/anti joins whose inputs vary: the selectivities replayed in each scenario), `querylets` (with `pushed: true` for pushed-down clauses at an outer join), `outer_rows`, `inner_rows`, `rows_point` (the stored point estimate), `pair_point` (PostgreSQL's estimate from this pair), `rows` |
| `ppi` | a ParamPathInfo gets its row vector (S4/S5) | `rels`, `outer`, `kind` (`base`) (required_outer as rels), `tuples`, `sel_est` (selectivity of restrictions plus enforced join clauses), `querylets` (the rel's own, then the join querylets), `rows_point`, `rows` |
| `ppi` (kind `join`) | a parameterized join rel gets its ppi vector (S5) | `rels`, `outer`, `kind`, `jointype`, `pair` (the input paths' rels), `pair_param` (their required_outer), `pair_src` (`rel`/`ppi`/`const`: where each input vector comes from), `sel`, `sels`, `querylets`, `outer_rows`, `inner_rows`, `rows_point`, `rows` |
| `ecgraph` | a query level's equivalence classes with 3 or more member relations get their spanning-forest data, on first use (S5) | `pass`, `ecs`: list of `{members` (instance names, best-ranked first), `cover` (profiled pairs of each member), `order` (the profiled pairs' instances in Kruskal order)`}`, `pairs`: list of `{inst, nclauses` (one equality per class containing both), `key` (null: no profile), `sel_est`, `bin}` |
| `path` | a path is offered to `add_path` / `add_partial_path`, or is an input of one, and gets its cost vectors (S6) | `pass`, `rels`, `id` (unique within the query; inputs get theirs first), `type` (path type name), `children` (ids), `param` (rels or null), `npathkeys`, `partial`, `status` (`const`: no input varies; `replay`; `mismatch`: the replay with nothing changed did not reproduce the scalars, vectors kept constant; `unsupported`; `nocontext`: a join path costed outside its join), `detail` (index names, join type / clause counts / `mat`, Unique method, workers), `semi_frozen` (the semi-join factors were kept at the point), `replayed_rows`, `replayed_startup`, `replayed_total` (`mismatch` only: what the point replay gave), `rows_point`, `startup_point`, `total_point`, `rows`, `startup`, `total` |
| `env` | pass 1 records an envelope entry (S7) | `rels`, `s`, `cost` |
| `keep` | pass 2 finishes pruning a rel (S8) | `rels`, `cands` (ids), `kept` (ids), `obj` (objective value per candidate, same order as `cands`), `group` (feature group of each candidate) |
| `final` | the final plan is chosen (S9) | `id`, `obj` |

Querylet entries.  `{key, inst, sel_est, bin}` is a drawn querylet.  `{key: "ec", inst: "ec", num, den}` (in `rel` kind `join`, and `ppi` events) is the factor of the multi-member equivalence classes: the product of the multipliers of the pair instances in `num`, then divided by those in `den`, in list order; the pairs are those of the level's latest `ecgraph` event.  A join querylet whose instance ends in `+` holds the other clauses of a pair that is also an equivalence-class pair; it draws independently of that pair.

The `pass` field is the phase name: `base`, `envelope`, `main` or `upper` (`RobustPass` in `optimizer/robust.h`).

Vector conventions:

- A vector's length is 1 (constant) or N (`error_sample_count`).
- The `*_point` fields are the scalar fields: PostgreSQL's point estimate, never a mean.

## Invariants the harness checks

| Step | Invariant |
|---|---|
| S1 | Exactly one `query` event per planning, matching the GUCs. Every printed number parses back to the exact double, and `nan`/`inf`/`-inf` survive. |
| S2 | `profile` bins are bit-identical to an independent Python implementation of the rules in `robustprofile.c`, and so is the bin lookup. A cached profile is not re-emitted. |
| S3 | `seed` and `x` are bit-identical to a Python port of `pg_prng`/`hash_bytes_extended` and the rules in `robustsample.c`. With bandwidth 0, every `draw.x[s]` is one of the bin's log ratios. Draws are reproducible across runs, the draws for N are a prefix of those for a larger N, and the bandwidth does not change the points drawn. The two instances of a self-join have different `inst` and independent `x`. |
| S4/S5 | Plans equal stock (nothing reads the vectors yet). Trivial profile: `rows[s] == rows_point` exactly. Any profile: `rows[s]` equals the closed form of `optimizer/robustrows.h` recomputed from the profile files (bins, draws, multipliers `m = clamp01(sel·e^x)/sel`, then `clamp_row_est(tuples · clamp01(sel_est · Π m))`, min with the rel's rows for a ppi, children summed in order for an append rel). |
| S5 | Trivial profile: join rel `rows[s] == rows_point` (first pair) or `pair_point` (later pairs); replayed selectivities equal the point ones. Any profile: every join rel / join ppi vector equals `join_rows_at` of `robustrows.c` recomputed in Python from the input vectors, the (replayed) selectivities and the multipliers, and the input vectors equal the input rels' / ppis' vectors. A later pair's vector differs from the first pair's by no more than the `clamp_row_est` rounding bound, equivalence-class cliques and stars included (samples where a node is clamped to 1 or at the inner-join cap are skipped). With one-point profiles, every join event equals `pair_point` × M(J) of spanning forests built in Python. Scaling one filter by K gives the rows stock PostgreSQL estimates for the query with that filter K times as selective. |
| S6 | Plans equal stock (the vectors are not read yet). Trivial and all-ones profiles: every path is `replay` or `const`, and `rows[s]`, `startup[s]`, `total[s]` equal the points bit for bit, serial and parallel. Any profile: a non-partial scan or join path's `rows` is its rel's or ppi's vector; Material and Memoize follow their input (Memoize adds `cpu_tuple_cost`); inputs are dumped before the paths that use them. One-point profiles scaling a filter by K: every path the two plannings share has the rows, startup and total cost stock PostgreSQL computes for the query whose filter selects K times as much (relative 1e-6), unless the stock rows themselves differ (a join selectivity that moves with the filter, which S5 holds at the point). |
| S7 | Trivial profile: `env.cost` equals the stock cheapest total of `rels` for every `s`. Any profile: `total[s] >= env.cost(rels, s) * (1 - 0.01)` for every pass-2 path (T4). |
| S8 | `kept` equals what `robdp_dump.py` recomputes from the vectors and the envelope with numpy. |
