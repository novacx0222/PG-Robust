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

**Equivalence classes with 3 or more members.** PostgreSQL generates one clause per class between two sides, and which one depends on the pair (t1.a = t3.a or t2.a = t3.a). Querylets taken from those clauses made the vector depend on the pair that built the join rel (median relative difference 0.45 on cliques; v20 behaved the same). Such a class now enters as a factor instead:

- Every relation set X gets the edges of a spanning forest of the class's members in X, chosen among the member pairs that have a profile. Members are ranked by how many profiled pairs they are in, then by name. Profiled pairs are taken in the order (better-ranked endpoint, other endpoint) and kept when they join two components (Kruskal).
- M(X) is the product of the multipliers of X's forest edges. A pair shared by two classes (composite keys) counts once, and its querylet is all the classes' equalities between the two relations.
- A join gets M(J) / (M(outer) M(inner)). A parameterized relation X with outer R carries F(X | R) = M(X + R) / M(R): a base ppi gets that factor, and a join ppi gets F(join | req) / (F(outer | oreq) F(inner | ireq)).
- Other clauses between the two relations of a class pair form their own querylet with instance `a=b+`, which draws independently, so the pair's draws are not counted twice.

When some member has a profiled pair with every other member, the forest is the star around that member. On the real JOB profiles (39 classes with 3 or more members), the best-covered star covers every needed edge in each class that has any profile, against 79% for the star around the smallest name. When the profiled pairs form a path (a=b, b=c, a=d, c=e), every star misses two of them, while the forest uses all four (J9).

The factor depends on X and its inputs' relation sets only, so a join rel's vector no longer depends on the pair that builds it, up to `clamp_row_est` rounding. The exception is a scenario where a factor such as m12 · m13 / m23 pushes a join's selectivity above 1, so that PostgreSQL's cap rows ≤ outer · inner applies. Such a scenario is not physically consistent (it would need |t1 t2 t3| > |t1| · |t2 t3|). It occurred in the synthetic tests, where `t.a` joins have selectivity 0.01. JOB's foreign-key joins have selectivities around 1e-6, so they would need a factor over 10^6.

**Path inputs.** A parameterized join's input vector is the path's ppi vector, or the rel vector for an unparameterized path. Any other path, such as a unique-ified path whose rows are a distinct count, enters as a constant, since paths have no vectors before S6.

## Tests

| Test | Result |
|---|---|
| `make check`, debug + release | 215/215 |
| `make check` with robust planning and the dump on, debug + cassert | 214/215, only sysviews (`enable_rows_dist = on`). Every plan is unchanged; no assertion failure with all later pairs evaluated; 74 equivalence-class graphs built. |
| `test_robust` module | pass |
| J1: robust on with the dump on (later pairs evaluated), 3 profile kinds | 75/75 plans identical to stock |
| J2: trivial and ones profiles | 7186 join rel / join ppi events have rows[s] equal to the point. All 8 semi/anti replays reproduce the point selectivities exactly. |
| J3: Python recomputation from the input vectors, the selectivities (replayed ones from `sels`) and the multipliers (S3/S4 reference) | 17,965 events bit-identical over 5 configurations, with 10,779 varying vectors. Input vectors equal the vectors of the input rels and ppis. |
| J4: pair independence, against the rounding bound Σ 0.5 / rows over both join trees | Other queries: 8991 (pair, s), 0 outside the bound (worst at 0.91 of it; 1870 skipped where a node was clamped to 1). Clique and star queries (q10, q11, q12, q20): 344,100 (pair, s), 0 outside (worst at 0.998 of it; 27,730 skipped where a node was clamped to 1 or at the inner-join cap). Before the change, their median relative difference was 0.45. |
| J5: join-type bounds on every event | hold |
| J6: replay against PostgreSQL. A profile scaling t1's or t2's filter by K ∈ {0.5, 2, 3} vs stock PostgreSQL on the query whose filter selects K times as much | 21/21 equal for inner, left, full, semi and anti joins (e.g. anti: 12000 = stock; without the replay it would stay 16000) |
| J9: equivalence classes on one-point profiles (exact multipliers), C forests vs a separate Python implementation | 142 join events of all pairs equal pair_point × M(J). Path a=b ×2, b=c ×0.5, a=d ×4, c=e ×3: forest uses all four edges, M = 12 (any star: 2 edges). Hub t3 (not the smallest name): star at t3, M = 4. Two classes sharing the pair t1=t2: counted once, M = 30 (60 if counted twice). Extra clause t1.b < t2.b on the pair t1=t2: drawn as `t1=t2+`, M = 12. |
| J7: S4 gates (and S3, S2, S1 through them) | pass |
| J8: planning time, robust on / off | geometric mean 1.09 (debug) – 1.21 (release), max 1.47. The dump is off for this measurement. |

`python3 check_s5.py [--install v2-debug]` reruns everything in about 30 s.

## Problems found while testing

- **N = 1.** Whether a vector varies was judged by its length, but with N = 1 every vector has length 1, so joins treated their inputs as the point estimate. It is now judged by whether `robust_rows` / `ppi_robust_rows` is NULL; `robust_path_rows()` reports it. J6 (run with N = 1) found this.
- **Semi and anti joins** ignored errors on their inner side; this was a model gap, fixed by the replay described above.
- **Harness integers.** JSON integers load as Python ints, whose exact products differ from C doubles above 2^53, which gave a false J5 failure on clique8. `vec()` now returns floats.
- **J4's first metric**, plain relative difference, was dominated by integer rounding at tiny intermediate rows (2 vs 3). It was replaced by the rounding bound.
- `foreach_ptr` exists only from PostgreSQL 17.

## Decision: equivalence classes with 3 or more members

Options considered: (1) PostgreSQL's generated clauses of the first pair (v20), (2) a canonical spanning tree, (3) all pairs symmetrically, M(J) = (Π m_pair)^(2/k). Option 3 shrinks the variance of log M to 2/k of a tree's under independent errors (a third for k = 6) and mixes in pairs without profiles. The chosen version of option 2 picks the tree by profile coverage (above), which on the real JOB profiles is the best-covered star and reproduces the context the edges were measured in: when one side already holds the hub, the join's factor is the multiplier of the edge to the new relation.
