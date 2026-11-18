# S5 results (2026-11-18): row vectors of join relations and parameterized joins

## What was added

| Hook | Vector |
|---|---|
| `set_joinrel_size_estimates` (join rels and partitionwise child joins, from the first pair) | `joinrel->robust_rows` |
| `get_joinrel_parampathinfo` (through the new `get_parameterized_joinrel_size_sel`) | `ppi->ppi_robust_rows` |
| `build_join_rel`, existing joinrel, dump on only | a `rel` event for every later pair (not stored) |
| `mark_dummy_rel` | cleared |

`calc_joinrel_size_estimate()` now hands out its three selectivities (fkselec, jselec, pselec; `RobustJoinSel` in `cost.h`), and `robust_calc_joinrel_size()` exposes it. The scalar `rows` and `ppi_rows` are unchanged.

## Model

In scenario s, `calc_joinrel_size_estimate()` gets the input vectors o[s] and i[s] and PostgreSQL's selectivities scaled by the querylet multipliers. The multipliers of the join's own clauses are folded into fkselec (Mj), and those of pushed-down clauses at an outer join into pselec (Mp); a scaled selectivity is capped at 1.

| Join | rows[s] |
|---|---|
| inner | min(o·i·fk'·j, o·i) |
| left | max(min(o·i·fk'·j, o·i), o) · p' |
| full | max(min(…), o, i) · p' |
| semi | min(o·fk'·j, o) |
| anti | max(o·(1 − fk'·j), 0) · p' |

Here fk' = fkselec·Mj and p' = min(pselec·Mp, 1). Each expression keeps the shape it has in `costsize.c`. With every multiplier equal to 1, the result is bit-identical to PostgreSQL's, including the multiply-add that clang fuses in the anti-join formula. A parameterized join applies the same formula to its input paths' vectors and caps the result by the join rel's rows[s], as `get_parameterized_joinrel_size()` does.

**Semi and anti joins: replay.** `eqjoinsel_semi` depends on the inner side's size: nd2 is capped by the inner rel's rows and by the inner base rel's rows, and the fraction by inner rows × inner selectivity. With multipliers alone, an error in the inner side's filter would never reach a semi or anti join (v20 had this problem too). So for these joins, when an input varies, each scenario sets the `rows` fields of the joined rels and their base rels to the scenario's values and calls PostgreSQL's estimator again. It uses fresh copies of the RestrictInfos, so the cached selectivities are neither used nor changed. Inner, left and full joins don't need this, because their selectivities do not depend on input rows.

**Pair dependence.** Like rel->rows, the vector comes from the pair that first builds the join rel.

- With equivalence classes of at most 2 members (chains, outer joins), every pair gives the same vector up to `clamp_row_est` rounding.
- With an equivalence class of 3 or more members (cliques, stars), the clause PostgreSQL generates for a pair depends on the pair: t1.a = t3.a or t2.a = t3.a. So do the querylet, its draws and the vector: the median relative difference is 0.45. v20 behaved the same. A canonical choice of the class's querylets would remove this (see Open decision below).

**Path inputs.** A parameterized join's input vector is the path's ppi vector, or the rel vector for an unparameterized path. Any other path, such as a unique-ified path whose rows are a distinct count, enters as a constant, since paths have no vectors before S6.

## Tests

| Test | Result |
|---|---|
| `make check`, debug + release | 215/215 |
| `make check` with robust planning and the dump on, debug + cassert | 214/215, only sysviews (`enable_rows_dist = on`). Every plan is unchanged; no assertion failure with all later pairs evaluated. |
| `test_robust` module | pass |
| J1: robust on with the dump on (later pairs evaluated), 3 profile kinds | 75/75 plans identical to stock |
| J2: trivial and ones profiles | 7186 join rel / join ppi events have rows[s] equal to the point. All 8 semi/anti replays reproduce the point selectivities exactly. |
| J3: Python recomputation from the input vectors, the selectivities (replayed ones from `sels`) and the multipliers (S3/S4 reference) | 17,965 events bit-identical over 5 configurations, with 10,779 varying vectors. Input vectors equal the vectors of the input rels and ppis. |
| J4: pair independence, against the rounding bound Σ 0.5 / rows over both join trees | 8991 (pair, s) in queries without multi-member equivalence classes: 0 outside the bound (worst at 0.91 of it; 1870 skipped where a node was clamped to 1). Clique and star queries are informational: median relative difference 0.45. |
| J5: join-type bounds on every event | hold |
| J6: replay against PostgreSQL. A profile scaling t1's or t2's filter by K ∈ {0.5, 2, 3} vs stock PostgreSQL on the query whose filter selects K times as much | 21/21 equal for inner, left, full, semi and anti joins (e.g. anti: 12000 = stock; without the replay it would stay 16000) |
| J7: S4 gates (and S3, S2, S1 through them) | pass |
| J8: planning time, robust on / off | geometric mean 1.10–1.22, max 1.6 on small queries. The dump is off for this measurement. |

`python3 check_s5.py [--install v2-debug]` reruns everything in about 30 s.

## Problems found while testing

- **N = 1.** Whether a vector varies was judged by its length, but with N = 1 every vector has length 1, so joins treated their inputs as the point estimate. It is now judged by whether `robust_rows` / `ppi_robust_rows` is NULL; `robust_path_rows()` reports it. J6 (run with N = 1) found this.
- **Semi and anti joins** ignored errors on their inner side; this was a model gap, fixed by the replay described above.
- **Harness integers.** JSON integers load as Python ints, whose exact products differ from C doubles above 2^53, which gave a false J5 failure on clique8. `vec()` now returns floats.
- **J4's first metric**, plain relative difference, was dominated by integer rounding at tiny intermediate rows (2 vs 3). It was replaced by the rounding bound.
- `foreach_ptr` exists only from PostgreSQL 17.

## Open decision: equivalence classes with 3 or more members

The options for the querylets of a multi-member class in a join rel are:

1. Keep PostgreSQL's generated clauses of the first pair (now; v20).
2. A canonical spanning tree, for example a star around the member with the smallest instance name. The multiplier at each join is M(J) / (M(outer) · M(inner)).
3. All pairs of the class within J, symmetrically: M(J) = (Π m_pair)^(2/k).

Options 2 and 3 make the vector independent of the join order, which matters for the S7 envelope C★(U, s).
