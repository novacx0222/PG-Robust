/*-------------------------------------------------------------------------
 *
 * robustvec.h
 *	  Per-sample vectors of RobDP robust planning.
 *
 * Robust planning (RobDP) evaluates row counts and costs at N sample
 * locations of the true selectivities, N = error_sample_count.  A RobustVec
 * holds such a quantity for every sample.  Its length is either N, or 1 for
 * a quantity that does not depend on the sample; a length-1 vector reads
 * the same value for every sample (robust_vec_at).  No other length is
 * valid (robust_vec_check).
 *
 * The scalar field a vector accompanies (rel->rows, path->total_cost, ...)
 * always keeps PostgreSQL's point estimate; it is not derived from the
 * vector.
 *
 * src/include/optimizer/robustvec.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ROBUSTVEC_H
#define ROBUSTVEC_H

/* Upper limit of error_sample_count */
#define ROBUST_MAX_SAMPLES	1000

typedef struct RobustVec
{
	int			n;				/* 1 (same for all samples) or N */
	double		v[FLEXIBLE_ARRAY_MEMBER];
} RobustVec;

extern RobustVec *robust_vec_make(int n);
extern RobustVec *robust_vec_const(double x);
extern RobustVec *robust_vec_copy(const RobustVec *src);
extern void robust_vec_check(const RobustVec *v, int nsamples);
extern bool robust_vec_varies(const RobustVec *v);
extern double robust_vec_mean(const RobustVec *v);
extern void robust_vec_clamp_rows(RobustVec *v);

/*
 * robust_vec_at
 *		The value of "v" at sample s.
 */
static inline double
robust_vec_at(const RobustVec *v, int s)
{
	Assert(v != NULL && v->n >= 1);
	if (v->n == 1)
		return v->v[0];
	Assert(s >= 0 && s < v->n);
	return v->v[s];
}

#endif							/* ROBUSTVEC_H */
