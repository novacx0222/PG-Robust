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
#include "optimizer/robustvec.h"

/*
 * A querylet whose errors are drawn from a profile.  Querylets without a
 * profile are not represented (their multiplier is 1).
 */
typedef struct RobustQuerylet
{
	char	   *key;			/* the profile used */
	char	   *inst;			/* instance name, seeds the draws */
	double		sel_est;		/* PostgreSQL's estimate of the querylet */
	int			bin;			/* the profile bin of sel_est */
	RobustVec  *x;				/* the draws, length N */
	RobustVec  *mult;			/* m_q, length N */
} RobustQuerylet;

/* The row-count vector of a relation: rel->robust_rows, or rel->rows */
extern RobustVec *robust_rel_rows(const RelOptInfo *rel);

/* Querylets */
extern RobustQuerylet *robust_base_querylet(PlannerInfo *root,
											RelOptInfo *rel, double sel_est);
extern List *robust_join_querylets(PlannerInfo *root, List *clauses);

/* Row counts */
extern void robust_set_baserel_rows(PlannerInfo *root, RelOptInfo *rel,
									Selectivity sel);
extern void robust_set_appendrel_rows(PlannerInfo *root, RelOptInfo *rel);
extern void robust_set_baserel_ppi_rows(PlannerInfo *root, RelOptInfo *rel,
										ParamPathInfo *ppi, Selectivity sel);

#endif							/* ROBUSTROWS_H */
