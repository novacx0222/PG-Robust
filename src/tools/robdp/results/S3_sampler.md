# S3 results (2026-11-18): drawing errors from profiles

## What was added

`optimizer/util/robustsample.c` (declared in `optimizer/robustprofile.h`):

- `robust_draw_errors(prof, inst, sel_est, &bin)` returns N = `error_sample_count` errors x_s = log(sel_true / sel_est) for one querylet instance, from the bin that `sel_est` falls in.
- `robust_draw_seed(key, inst)` and `robust_draw_bandwidth(prof, b)` are the two pieces it uses.

With `robust_debug_dump` on, each call emits a `draw` event: key, inst, seed, sel_est, bin, h, x.

The module function `test_robust_draw(key, inst, sel_est)` exposes the draw to SQL for tests. Nothing in the planner calls it yet; S4 does.

## Rules, and how they differ from v20

| Point | v2 | v20 |
|---|---|---|
| One draw | A point of the bin, chosen uniformly; plus h·Z when h > 0 (Gaussian KDE) | Same |
| Bandwidth h | `error_sample_kde_bandwidth` > 0: that value. = 0: no noise (bootstrap). < 0: the bin's Silverman h. | Same |
| Seed | hash_combine64(hash_bytes_extended(key, error_sample_seed), hash_bytes_extended(inst, 0)) | The same, plus the bin |
| Streams | Two pg_prng streams: one picks the points, one makes the noise (seeded by hash_combine64(seed, "noise")) | One stream for both |
| Consequences | The draws for N are a prefix of those for a larger N. Changing the bandwidth changes only the noise, not which points are drawn. Estimates of one instance that fall in different bins use the same random numbers, so only the points they select differ (common random numbers). | Changing the bandwidth or the bin re-drew everything |
| Floating point | point + noise is written as separate statements, so it cannot become an FMA | Single expression |

Instance keys (`inst`) are built in S4/S5: the alias for a base relation, `a=b` for the join clauses between a and b. The two sides of a self-join therefore get independent draws from the same profile.

## Tests

| Test | Result |
|---|---|
| `make check`, debug + release | 215/215 |
| `test_robust` module, debug + release | pass. New cases: the first 10 draws in bins 0 and 1 are pinned (rounded to 6 decimals, so the output does not depend on libm's last bit); reproducibility; the prefix property; bandwidth 0 draws only bin points and every point gets drawn; x ≠ y for two instances; a draw in the same bin at another estimate is unchanged; a new seed gives new draws; a bandwidth of 1e-9 moves each draw by less than 1e-7; Silverman smooths; a missing profile returns NULL. |
| D1: `seed` vs a Python port of `hash_bytes_extended`, `hash_combine64` | 120/120 identical. Instance keys of length 0..40 (every tail length of the hash), UTF-8, quotes; error_sample_seed 0, 42, 2^31−1. |
| D2: `x` vs a Python port of `pg_prng` (splitmix64 seeding, xoroshiro128**, `uint64_range` rejection, Box–Muller) and the drawing rules | 6088 draws, 396,400 values, all bit-identical. 8 profiles × B ∈ {1, 3, 8} × bandwidth {0, 0.25, Silverman} × 3 seeds × N = 20, plus N = 1 and N = 1000 configurations; estimates across every bin. |
| D3: bandwidth 0 | every value is one of its bin's log ratios |
| D4: properties (C output only) | reproducible across sessions; N = 10 is a prefix of N = 1000; a bandwidth of 1e-9 keeps the points; bins 0 and 3 of e_big (5000 points each) draw identical index sequences |
| D5: statistics, N = 1000 | corr(x, y) of a self-join = +0.051 (bound 0.126). KS distance of bootstrap draws to the bin: 0.026 and 0.026 (bound at α = 0.01: 0.052). KDE mean z = +0.83, +0.36, +0.46 and variance / (bin var + h²) = 0.980, 0.969, 0.969. |
| D6: S2 gates (P1–P5, which include the S1 gates) | pass |

`python3 check_s3.py [--install v2-debug]` reruns the gates in about 5 s.

## Problems found while testing

- `construct_array_builtin()` does not support float8 in PG 16. The test function uses `construct_array()` instead.
- The first D4 check used estimates 0.01 and 0.5 for t1, which both fall in its last bin. The check now takes the estimates from the computed bin boundaries of e_big, whose bins have equal sizes; the property holds only between bins of equal size.
