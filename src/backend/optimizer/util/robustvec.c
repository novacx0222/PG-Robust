/*-------------------------------------------------------------------------
 *
 * robustvec.c
 *	  Per-sample vectors of RobDP robust planning (see robustvec.h).
 *
 * src/backend/optimizer/util/robustvec.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/optimizer.h"
#include "optimizer/robustvec.h"

/*
 * robust_vec_make
 *		A new vector of length n, all zeros.
 */
RobustVec *
robust_vec_make(int n)
{
	RobustVec  *v;

	if (n < 1 || n > ROBUST_MAX_SAMPLES)
		elog(ERROR, "RobDP: invalid vector length %d", n);
	v = palloc0(offsetof(RobustVec, v) + sizeof(double) * n);
	v->n = n;
	return v;
}

/*
 * robust_vec_const
 *		A length-1 vector: the same value x at every sample.
 */
RobustVec *
robust_vec_const(double x)
{
	RobustVec  *v = robust_vec_make(1);

	v->v[0] = x;
	return v;
}

RobustVec *
robust_vec_copy(const RobustVec *src)
{
	RobustVec  *v;

	if (src == NULL)
		return NULL;
	v = robust_vec_make(src->n);
	memcpy(v->v, src->v, sizeof(double) * src->n);
	return v;
}

/*
 * robust_vec_check
 *		Raise an error unless "v" is a valid vector for "nsamples" samples:
 *		not NULL and of length 1 or nsamples.
 *
 * A vector of any other length would be read past its end, or would
 * silently use only part of the samples, so this is an error, not an
 * assertion.
 */
void
robust_vec_check(const RobustVec *v, int nsamples)
{
	if (v == NULL)
		elog(ERROR, "RobDP: missing per-sample vector");
	if (v->n != 1 && v->n != nsamples)
		elog(ERROR, "RobDP: vector of length %d, expected 1 or %d",
			 v->n, nsamples);
}

/*
 * robust_vec_varies
 *		Does the value differ between samples?
 *
 * A vector of length N whose components are all equal does not vary.
 */
bool
robust_vec_varies(const RobustVec *v)
{
	for (int i = 1; i < v->n; i++)
	{
		if (v->v[i] != v->v[0])
			return true;
	}
	return false;
}

/*
 * robust_vec_mean
 *		Arithmetic mean of the components (diagnostics and objectives only;
 *		scalar fields hold point estimates, never this).
 */
double
robust_vec_mean(const RobustVec *v)
{
	double		sum = 0.0;

	for (int i = 0; i < v->n; i++)
		sum += v->v[i];
	return sum / v->n;
}

/*
 * robust_vec_clamp_rows
 *		Apply clamp_row_est() to every component, in place.
 *
 * Every row estimate PostgreSQL computes is clamped (at least 1, an
 * integer, at most MAXIMUM_ROWCOUNT), and its cost functions rely on that.
 * The row count at each sample follows the same rule, so that sample s is
 * costed exactly as PostgreSQL would cost it if the true row counts were
 * those of sample s.
 */
void
robust_vec_clamp_rows(RobustVec *v)
{
	for (int i = 0; i < v->n; i++)
		v->v[i] = clamp_row_est(v->v[i]);
}
