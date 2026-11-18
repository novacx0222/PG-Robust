# S2 results (2026-11-18): error profile loading and binning

## What was added

`optimizer/robustprofile.h` and `optimizer/util/robustprofile.c`:

- `robust_get_profile(key)` reads `<error_profile_path>/<key>.txt` on first use and caches it for the session.
- `robust_profile_find_bin(prof, sel_est)` picks the bin an estimate draws its errors from.

The module function `test_robust_profile(key, sel_est[])` exposes both for tests.

## Behavior, and how it differs from v20

| Point | v2 | v20 |
|---|---|---|
| When profiles load | Lazily, one file per key. The file is reloaded when it changes (inode, size, mtime), or when `error_profile_path` or `error_bin_count` changes. | All `*.txt` files were loaded in the GUC assign hook. Later changes to the bin count did not take effect. |
| Invalid lines | WARNING with file and line number (the first 5, then a total). Empty lines and `#` comments are allowed. A file with no valid point is ignored with a WARNING. | Skipped silently. |
| Size | Unlimited (dynamic arrays) | Silently truncated at 1024 points |
| Key safety | Keys that are empty, start with `.`, or contain `/` or `\` are rejected. A quoted alias such as `"../x"` cannot make the server read outside the directory. | Not checked |
| Sort order | `(sel_est, x)`, so the order is deterministic and independent of the file order | `sel_est` only, with qsort's unstable tie order |
| Equal estimates at a bin boundary | Never split. The boundary moves forward past them, so every point can be drawn. | Split. A point with the same sel_est in the later bin was unreachable; with an all-equal profile, every bin but the first was unreachable. |
| Statistics | mean, sd (n−1) and Silverman h per bin. The squared-deviation sum is written so the compiler cannot fuse it into an FMA, which makes it bit-reproducible. | Same formulas |

## Tests

| Test | Result |
|---|---|
| `make check`, debug + release | 215/215 |
| `test_robust` module, debug + release | pass. New cases: 6 valid / 6 invalid lines with warnings, by-hand bin values for 2 and 4 bins (including the tie fix), cache reuse, rebuild on a bin-count change, missing / empty / unsafe keys, no directory configured. |
| P1: bins vs the Python reference, 25 profiles × B ∈ {1, 2, 3, 5, 8} (475 bins) | all fields bit-identical: first, n, lo, hi, mean, sd, h |
| P2: bin lookup at 497,780 estimates (every point, midpoints, nextafter neighbors, 0, 1) | identical |
| P3: edge cases | the same points in shuffled order give identical bins; an all-equal profile stays in one bin; no boundary splits equal estimates; 7 invalid lines give 5 warnings and n = 2; one point with 8 bins gives exactly one non-empty bin |
| P4: cache | reused without a reload; reloaded on file change, bin-count change and directory change |
| P5: S1 gates | G1–G5 pass. Planning still matches stock 25/25 with robust planning off and on. |

The test profiles included 5 lognormal profiles of 257 points (plus 15 pair profiles), a 20,000-point file, ties, a single point, three points, `sel_true = 0`, CRLF line endings, comments, invalid lines, and the same points in a shuffled order.

## Found and fixed during S2

- The first P1 run differed from Python by 1 ulp in sd and h. clang on arm64 fuses `ss += d*d` into an FMA. The C code now uses separate statements; the bins are bit-identical after that.
- My P3 expectation "a single point lands in bin 0" was wrong. Equal-count cutting puts it in bin 3 of 8, because `llround(0.5) = 1`. The C code and the Python reference already agreed.
- The bench database still had the S1 version of the `test_robust` extension. The checks now drop and recreate it.
