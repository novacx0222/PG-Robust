/*-------------------------------------------------------------------------
 *
 * robustrows.h
 *	  Row counts of relations in the error scenarios of RobDP.
 *
 * Scenario s (s = 0 .. N-1, N = error_sample_count) assigns every querylet
 * q -- the restriction clauses of a base relation, or the join clauses
 * between two base relations -- a true selectivity
 *
 *		sel_true_q[s] = clamp01(sel_q * exp(x_q[s]))
 *
 * where sel_q is PostgreSQL's estimate of q alone and x_q[s] a draw from
 * q's error profile (robust_draw_errors).  Any estimate PostgreSQL makes of
 * a clause list containing querylets q1, q2, ... becomes, in scenario s,
 *
 *		sel[s] = clamp01(sel * m_q1[s] * m_q2[s] * ...),
 *		m_q[s] = sel_true_q[s] / sel_q	(1 if sel_q = 0)
 *
 * and a row count is PostgreSQL's formula applied to sel[s] and to the
 * input row counts of scenario s.  A querylet without a profile has
 * m_q = 1, so with no profile, or with a profile whose errors are all 0,
 * every row count equals PostgreSQL's estimate exactly.
 *
 * The scalar row counts (rel->rows, ppi->ppi_rows) stay PostgreSQL's point
 * estimates.  The vectors are separate fields, NULL when the row count is
 * the same in every scenario.
 *
 * src/include/optimizer/robustrows.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ROBUSTROWS_H
#define ROBUSTROWS_H

#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/robustvec.h"

/*
 * A querylet whose errors are drawn from a profile.  Querylets without a
 * profile are not represented (their multiplier is 1).
 *
 * The factor of the multi-member equivalence classes at a join is also
 * carried as a RobustQuerylet, with key "ec", no draws of its own (x NULL)
 * and mult = the product of the pair querylets in ec_num divided by those
 * in ec_den (see "Equivalence classes" below).
 */
typedef struct RobustQuerylet
{
	char	   *key;			/* the profile used, or "ec" */
	char	   *inst;			/* instance name, seeds the draws */
	double		sel_est;		/* PostgreSQL's estimate of the querylet */
	int			bin;			/* the profile bin of sel_est */
	RobustVec  *x;				/* the draws, length N */
	RobustVec  *mult;			/* m_q, length N */
	bool		pushed;			/* at an outer join: all its clauses are
								 * pushed down (it scales pselec, not jselec) */
	List	   *ec_num;			/* "ec": pair querylets multiplied, */
	List	   *ec_den;			/* and divided, in this order */
	Index		ia;				/* instance relid of the base relation, or */
	Index		ib;				/* of the pair ia < ib; 0 for none / "ec" */
} RobustQuerylet;

/*
 * Equivalence classes.  For an equivalence class with k >= 3 member
 * relations, the clause PostgreSQL generates between two sides depends on
 * the pair of sides (t1.a = t3.a or t2.a = t3.a), so querylets taken from
 * the generated clauses would make a join relation's vector depend on the
 * pair that built it.  Instead, every relation set X gets the edges of a
 * spanning forest of the class's members in X, chosen from the member pairs
 * that have a profile:
 *
 *	- members are ranked by the number of profiled pairs they are in
 *	  (more first), then by instance name;
 *	- profiled pairs are taken in the order (better-ranked endpoint, other
 *	  endpoint) and kept if they join two components (Kruskal).
 *
 * When some member has a profiled pair with every other member, this is
 * the star around the best-covered member.  M(X) is the product of the
 * multipliers of the edges in X's forests (a pair shared by two classes
 * counts once), and a relation of sets X built from inputs Y, Z gets the
 * factor M(X) / (M(Y) M(Z)), which depends on X, Y and Z only.  Member pairs
 * without a profile have multiplier 1.
 *
 * This is used with robust_ec_forest on.  By default (off: clause mode, as
 * v20 and PAR2QO) the classes get no forests: the clause PostgreSQL
 * generates between the two sides of a join is a join querylet like any
 * other, so a join relation's errors are those of the member pairs its
 * first-built pair's clauses join, the same clauses PostgreSQL's estimate
 * of it uses.  A forest edge's multiplier m = sel_true / sel_est of its own
 * pair instead scales PostgreSQL's estimate of whichever clause the first
 * pair generated, whose sel_est can differ by 5x (t.id = x.movie_id against
 * x.movie_id = y.movie_id on JOB), so a perfect profile would not give the
 * true row count.
 */
typedef struct RobustECGraph RobustECGraph;

/* The row-count vector of a relation: rel->robust_rows, or rel->rows */
extern RobustVec *robust_rel_rows(const RelOptInfo *rel);
extern RobustVec *robust_path_rows(const Path *path, const char **source,
								   bool *varies);

/* Querylets */
extern RobustQuerylet *robust_base_querylet(PlannerInfo *root,
											RelOptInfo *rel, double sel_est);
extern List *robust_join_querylets(PlannerInfo *root, List *clauses);
extern List *robust_join_querylets_at(PlannerInfo *root, List *clauses,
									  Relids joinrelids);
extern RobustQuerylet *robust_ec_factor(PlannerInfo *root,
										Relids *num, int nnum,
										Relids *den, int nden, bool pushed);
extern bool robust_ec_is_star(PlannerInfo *root, EquivalenceClass *ec);
extern List *robust_join_querylets_ec(PlannerInfo *root, RelOptInfo *joinrel,
									  SpecialJoinInfo *sjinfo, List *clauses,
									  Relids *num, int nnum,
									  Relids *den, int nden);

/* Row counts */
extern void robust_set_baserel_rows(PlannerInfo *root, RelOptInfo *rel,
									Selectivity sel);
extern void robust_set_appendrel_rows(PlannerInfo *root, RelOptInfo *rel);
extern void robust_set_baserel_ppi_rows(PlannerInfo *root, RelOptInfo *rel,
										ParamPathInfo *ppi, Selectivity sel);
extern void robust_set_joinrel_rows(PlannerInfo *root, RelOptInfo *joinrel,
									RelOptInfo *outer_rel,
									RelOptInfo *inner_rel,
									SpecialJoinInfo *sjinfo,
									List *restrictlist,
									const RobustJoinSel *sel, bool store);
extern void robust_set_joinrel_ppi_rows(PlannerInfo *root,
										RelOptInfo *joinrel,
										ParamPathInfo *ppi,
										Path *outer_path, Path *inner_path,
										SpecialJoinInfo *sjinfo,
										List *restrict_clauses,
										const RobustJoinSel *sel);

#endif							/* ROBUSTROWS_H */
