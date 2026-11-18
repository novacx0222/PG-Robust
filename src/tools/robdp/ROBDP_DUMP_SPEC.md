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
- A relation is named by `"rels"`: a space-separated, sorted list of range-table aliases, for example `"t1 t2"`. Outer-join relids are left out.

## Events

| `ev` | Emitted when | Fields |
|---|---|---|
| `query` | `standard_planner` starts (S1) | `enabled`, `N`, `seed`, `bins`, `bandwidth`, `profile_path` |
| `profile` | a profile is loaded or reloaded (S2) | `key`, `path`, `n` (valid points), `skipped` (invalid lines), `bins`: list of `{first, n, lo, hi, mean, sd, h}` |
| `draw` | a querylet's samples are drawn (S3) | `key` (profile key), `inst` (instance key), `seed` (16 hex digits), `sel_est`, `bin`, `h` (bandwidth used), `x` (list of N) |
| `rel` | a rel gets its row vector (S4/S5) | `pass`, `rels`, `kind` (`base`/`join`/`append`/`other`), `tuples`, `sel_est` (base only), `querylets` (list of `{key, sel_est, bin}`), `rows_point`, `rows` (list of 1 or N), `pair` (join only: `[outer rels, inner rels]` of the pair that built it) |
| `ppi` | a ParamPathInfo gets its row vector (S4/S5) | `rels`, `outer` (required_outer as rels), `rows_point`, `rows` |
| `path` | a path is costed in pass 0/2 (S6) | `pass`, `rels`, `id` (unique within the query), `type` (path type name), `children` (ids), `param` (rels or null), `npathkeys`, `partial`, `rows_point`, `startup_point`, `total_point`, `rows`, `startup`, `total` |
| `env` | pass 1 records an envelope entry (S7) | `rels`, `s`, `cost` |
| `keep` | pass 2 finishes pruning a rel (S8) | `rels`, `cands` (ids), `kept` (ids), `obj` (objective value per candidate, same order as `cands`), `group` (feature group of each candidate) |
| `final` | the final plan is chosen (S9) | `id`, `obj` |

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
| S4/S5 | Trivial profile: `rows[s] == rows_point` exactly, and `rows_point` equals the stock estimate. Shift / two-point profiles: `rows[s]` equals the closed form computed from `tuples`, `sel_est` and `x`. |
| S5 | For inner-join chains, `rows[s]` of a joinrel does not depend on which `pair` built it, up to `clamp_row_est` rounding. |
| S6 | Trivial profile: `startup[s] == startup_point` and `total[s] == total_point`, and the points equal stock costs. |
| S7 | Trivial profile: `env.cost` equals the stock cheapest total of `rels` for every `s`. Any profile: `total[s] >= env.cost(rels, s) * (1 - 0.01)` for every pass-2 path (T4). |
| S8 | `kept` equals what `robdp_dump.py` recomputes from the vectors and the envelope with numpy. |
