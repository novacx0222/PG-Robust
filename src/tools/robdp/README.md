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
| `robdp_dump.py [DUMP.json]` | Parser for the planner's `ROBDP {json}` lines; without arguments it runs its self-test. The format is in `ROBDP_DUMP_SPEC.md`. |

## Typical gate

```bash
python3 explain_dump.py results/stock_v2rel.json --install v2-release      # once
python3 explain_dump.py results/cand.json --install v2-release --set enable_rows_dist=on ...
python3 compare.py results/stock_v2rel.json results/cand.json -v
```

Profiles live in `/Users/xuanchen2026/RobDP/data/profiles/<kind>`.

GUC order matters for the old p0 code. Set `error_sample_count` and `error_bin_count` before `error_profile_path`, because the old code builds the bins when the path is assigned.
