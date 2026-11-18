# S1 results (2026-11-18): vector type, GUCs, dump, pass skeleton

## What was added

| File | Content |
|---|---|
| `include/optimizer/robustvec.h`, `optimizer/util/robustvec.c` | `RobustVec`: variable length, n = 1 (same for all samples) or N, at most 1000. Functions: `make`, `const`, `copy`, `at` (broadcasts a length-1 vector), `check` (raises an ERROR when the length is neither 1 nor N, or the vector is NULL), `varies`, `mean`, `clamp_rows` (`clamp_row_est` per component). |
| `include/optimizer/robust.h`, `optimizer/util/robust.c` | GUC variables, the `RobustPass` enum (`NONE`/`BASE`/`ENVELOPE`/`MAIN`/`UPPER`), and the dump writer: `ROBDP {json}` lines with `%.17g` numbers, `nan`/`inf` as strings, and JSON escaping. |
| `nodes/pathnodes.h` | `PlannerInfo.robust_pass` and `robust_round`. Zero means robust planning is not running. |
| `utils/misc/guc_tables.c` | GUCs (see below) |
| `plan/planner.c` | `standard_planner` emits the `query` event when `robust_debug_dump` is on. |
| `test/regress/expected/sysviews.out` | Adds the `enable_rows_dist` line, because that test lists every `enable_*` GUC. |
| `test/modules/test_robust` | C unit tests and their regression test. |

GUCs:

| GUC | Context | Default | Range | Change from v20 |
|---|---|---|---|---|
| `enable_rows_dist` | user | off | | |
| `error_sample_count` | user | 20 | 1..1000 | v20: 4..100 |
| `error_sample_seed` | user | 42 | 0..INT_MAX | |
| `error_bin_count` | user | 1 | 1..8 | |
| `error_sample_kde_bandwidth` | user | 0 | −1..10 | |
| `error_profile_path` | **superuser** | empty | | v20: USERSET, default `/opt/error_profile` |
| `robust_debug_dump` | superuser | off | | new |

With `enable_rows_dist` on, nothing reads these settings yet. That starts in S2.

## Tests

| Test | Result |
|---|---|
| `make check`, debug + release | 215/215 |
| `test_robust` module, debug + release | pass |
| G1: `enable_rows_dist=off` vs stock, 25 queries | 25/25 SAME |
| G2: `enable_rows_dist=on` with lognormal profiles vs stock | 25/25 SAME |
| G3: `query` dump events | exactly 1 per planning (4 per query with 1 + 3 EXPLAINs); fields equal the GUCs |
| G4: dump number format | 0.1, 1/3, 1e−300, NaN, ±inf, −0, INT64_MIN, a large double and an escaped string all round-trip exactly in Python |
| G5: planning time, robust off / stock | geometric mean 1.037 (min 0.82, max 1.84). These are timings of 0.01–5 ms, so this is noise, not a regression. |

`test_robust` unit checks (C):

- `make` zeroes the vector; lengths 0 and 1001 raise an ERROR.
- A const vector reads the same value at all 1000 samples.
- `copy` is independent of its source; `copy(NULL)` returns NULL.
- `check` accepts lengths 1 and N, and rejects other lengths and NULL.
- `varies`, including a full vector whose components are all equal.
- `mean`.
- `clamp_rows` is bit-identical to `clamp_row_est` on −5, 0, 0.4, 1, 1.5, 2.5, 3.5, 1e300, NaN and inf (rint rounds half to even).
- The dump format is fixed by the expected output.
- GUC ranges, and permission denied for an ordinary user on `error_profile_path` and `robust_debug_dump`.
