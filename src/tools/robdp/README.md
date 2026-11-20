# RobDP v2 test harness

| What | Where |
|---|---|
| Source | `/Users/xuanchen2026/RobDP/PG-Robust-v2`, a git worktree of PG-Robust on branch `robdp/v2`, started from `REL_16_2` |
| Harness (this directory) | `src/tools/robdp` in the worktree; `/Users/xuanchen2026/RobDP/tools/v2` is a symlink to it |
| Old code, for reference | `/Users/xuanchen2026/RobDP/PG-Robust` (`robdp/p0-v20`), built as `p0-debug` / `p0-release` |
| Builds | `build/v2-{debug,release}`, installed to `install/v2-{debug,release}` (same configure flags as p0) |
| Bench cluster | `data/v2-bench`, port 55501, database `robdp` |

All builds share the bench cluster. They are all 16.2 with the same catalog, so every build plans against identical statistics.

## Scripts

| Script | Purpose |
|---|---|
| `setup_db.py [--install X] [--recreate]` | Load `schema.sql`. Prints a statistics fingerprint, which should be identical on every load. |
| `profiles.py KIND DIR` | Synthetic error profiles: `trivial`, `ones`, `shift:K`, `twopoint:A,B`, `lognormal:MU,SD`. |
| `explain_dump.py OUT.json --install X --set g=v ...` | Plan every query in `queries/`. Records the JSON plan, the median planning time and the server messages. |
| `compare.py A.json B.json [-v]` | Per-query verdict: SAME, COST-DIFF, ROWS-DIFF, PLAN-DIFF or ERROR. Exit status 0 means all SAME. |
| `check_s1.py` | S1 gate: plans unchanged with robust off and on, `query` dump events, exact number format. |
| `check_s2.py` | S2 gate: profile parsing, binning and lookup equal the Python reference; cache reloads; S1 gates. |
| `check_s3.py` | S3 gate: seeds and draws bit-identical to a Python port of pg_prng and hash_bytes_extended; bootstrap draws are bin points; prefix, reproducibility and common-random-number properties; statistics; S2 gates. |
| `check_s4.py` | S4 gate: plans unchanged; trivial profiles give rows[s] == rows_point; base, partition, append and ppi row vectors bit-identical to a Python reference from the profile files; instance naming; S3 gates. |
| `check_s5.py` | S5 gate: join rel and join ppi vectors bit-identical to a Python recomputation; pair independence within the rounding bound (equivalence-class cliques and stars included); spanning forests of multi-member equivalence classes vs Python; join-type bounds; semi/anti replay equal to stock estimates; S4 gates. |
| `check_s6.py` | S6 gate: replayed path costs equal the points with trivial profiles (serial and parallel), wiring of path rows to rel/ppi vectors, one-point profiles equal to stock PostgreSQL costs of the scaled query; plans unchanged; S5 gates; planning time. |
| `check_s7.py` | S7 gate: join search with envelope and main passes. Trivial profiles: plans and envelope equal stock; the envelope pass leaves the main pass unchanged; one-point profiles: envelope equal to stock PostgreSQL's cheapest cost of the scaled query; envelope vs kept paths; no kept path dominated; S6 gates; planning time. |
| `check_s8.py` | S8 gate: selection by the objectives. Trivial profiles with selection reduced to sorting: plans equal stock; every selection recomputed in Python from the vectors and the envelope (scores, kept paths, why); pathlists, Gather on kept partial paths, child join rels; settings; retained vs native pairing; S7 gates; planning time. |
| `robdp_dump.py [DUMP.json]` | Parser for the planner's `ROBDP {json}` lines; without arguments it runs its self-test. The format is in `ROBDP_DUMP_SPEC.md`. |

## Typical gate

```bash
python3 explain_dump.py results/stock_v2rel.json --install v2-release      # once
python3 explain_dump.py results/cand.json --install v2-release --set enable_rows_dist=on ...
python3 compare.py results/stock_v2rel.json results/cand.json -v
```

Profiles live in `/Users/xuanchen2026/RobDP/data/profiles/<kind>`.

GUC order matters for the old p0 code. Set `error_sample_count` and `error_bin_count` before `error_profile_path`, because the old code builds the bins when the path is assigned.
