# S4 results (2026-11-18): row vectors of base relations and parameterized scans

## What was added

`optimizer/robustrows.h` and `optimizer/util/robustrows.c`, plus hooks in PostgreSQL:

| Hook | Vector |
|---|---|
| `set_baserel_size_estimates` (base relations, partitions, subquery / function / values / CTE rels) | `rel->robust_rows` |
| `set_append_rel_size` | `rel->robust_rows` = the live children's vectors summed in PostgreSQL's order |
| `get_baserel_parampathinfo` | `ppi->ppi_robust_rows` |
| `set_dummy_rel_pathlist` | cleared (rows = 0) |
| `reparameterize_path_by_child` | copied with `ppi_rows` |

Supporting changes:

- `get_parameterized_baserel_size_sel()` also returns the selectivity it estimated.
- `robust_rel_name()` gives relations names that are unique within a query level.

Nothing reads the vectors yet. The scalar `rows` and `ppi_rows` fields are PostgreSQL's point estimates, unchanged.

## Model (robustrows.h)

Scenario s gives every querylet q a multiplier

    m_q[s] = clamp01(sel_q · exp(x_q[s])) / sel_q        (1 if sel_q = 0, or if q has no profile)

where sel_q is PostgreSQL's estimate of q alone and x_q[s] its draw (S3). Every estimate of a clause list becomes `clamp01(sel · Π m_q[s])`, run through PostgreSQL's own formula:

| Relation | rows[s] |
|---|---|
| base / partition | `clamp_row_est(tuples · clamp01(sel · m_r[s]))` |
| append | Σ over live children of child rows[s] |
| parameterized base scan | `min(clamp_row_est(tuples · clamp01(sel_all · m_r[s] · Π m_join[s])), rel rows[s])` |

## Querylets, profiles, instances

| Point | v2 | v20 |
|---|---|---|
| Restriction querylet | A base relation's restriction clauses; none if it has no restriction | Drawn even without restrictions |
| Join querylet | Join clauses between exactly two base relations, any operator. Their own estimate is the inner-join selectivity (dummy inner SpecialJoinInfo), the same in every context, so they always use the same bin. | Var = Var clauses only |
| Profile key | alias, else the alias cut at its first digit (`mc1` → `mc`); `a=b` / `std(a)=std(b)` in sorted order | Same |
| Instance (seed) | `robust_rel_name`: the alias, or `alias#rti` if another range-table entry of the query level has the same alias; `ia=ib` for joins | The alias; two references with the same alias drew identical errors |
| Partitions | Use the topmost parent's querylet (its key and instance) with their own estimate; named `p:p1` in dumps | Child alias (equal to the parent's) |
| UNION ALL leaves | Their own alias | Same |
| ppi | Varies: restriction querylet times the join querylets of the enforced clauses | Constant |

Open point: two separately planned subqueries can still share an instance name if they use the same alias, because names are unique only within a query level.

## Tests

| Test | Result |
|---|---|
| `make check`, debug + release | 215/215 |
| `make check` with robust planning on (lognormal profiles, N = 20, 3 bins, Silverman), debug + cassert | 214/215. The only diff is sysviews showing `enable_rows_dist = on`: every regression plan is unchanged, with no assertion failure. |
| `test_robust` module | pass |
| R1: robust on with trivial, ones, shift:4, twopoint:0.1,10 and lognormal profiles | 125/125 plannings identical to stock |
| R2: trivial and ones profiles | 596 rel/ppi events, 580 querylets with a profile: rows[s] == rows_point for every s |
| R3: Python recomputation from the profile files: bins (S2 reference), draws (S3 reference), multipliers, rows | 2675 draw / rel / ppi events bit-identical; 5 configurations (bandwidth 0, fixed and Silverman; 1–4 bins; N = 1, 20, 50); 680 vectors vary |
| R4: instances | duplicate alias → `t1#1`, `t1#3`, independent draws; self-join x, y independent plus join querylet `x=y`; partitions `p:p1..p:p4` use instance `p`; append sums p1..p4, and only p1..p3 when p4 is pruned; partitioned self-join children draw as x and y; UNION ALL leaves keep t1, t2; a ppi combines the restriction and join querylets |
| R5: S3 gates (with S2 and S1) | pass |
| R6: planning time, robust on / off | geometric mean 1.03–1.09 (release), 1.05 (debug). Draws are recomputed for each ppi and each profile is `stat()`ed on every lookup; caching can wait for S6's timing gate. |

`python3 check_s4.py [--install v2-debug]` reruns all of this in about 10 s. Six extra queries (partition filters and pruning, duplicate alias, partitioned self-join, ppi with both querylet kinds, self-join with both sides filtered) are written to `data/s4_queries`.

## Problems found while testing

- `robustrows.c` was missing `optimizer/pathnode.h` (for `find_base_rel`); this was a compile error.
- R4 first expected `t1#4`. The pulled-up IN subquery's `t1` is rti 3 (the sublink's subquery RTE is replaced), and q50 filters only x, so y has no draw. The checks now name rti 3 and use a self-join that filters both sides.
