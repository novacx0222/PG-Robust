/*-------------------------------------------------------------------------
 *
 * robustprofile.h
 *	  Error profiles of RobDP robust planning.
 *
 * An error profile records how wrong PostgreSQL's selectivity estimates
 * were for one querylet (a relation's restrictions, or the join clauses
 * between two relations) on a training workload: a list of points
 * (sel_true, sel_est).  RobDP uses the error of a point,
 *
 *		x = log(sel_true / sel_est),
 *
 * as a draw from the distribution of the estimation error.  Since the error
 * depends on the size of the estimate, the points are split into
 * error_bin_count equal-count bins by sel_est, and a selectivity estimate
 * draws its errors from the points of its own bin.
 *
 * src/include/optimizer/robustprofile.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ROBUSTPROFILE_H
#define ROBUSTPROFILE_H

#include "optimizer/robust.h"

/*
 * Selectivities are clamped to at least this before taking the log ratio,
 * so a point whose true selectivity is 0 (an empty result) gets a large
 * but finite negative error.
 */
#define ROBUST_PROFILE_MIN_SEL	1e-12

/*
 * One bin: the points first .. first + n - 1 of the profile's sorted
 * arrays.  Its range of estimates is (lo, hi]; lo is the previous bin's hi
 * (-inf for the first bin) and hi is the largest sel_est in the bin.
 */
typedef struct RobustProfileBin
{
	int			first;
	int			n;
	double		lo;
	double		hi;
	double		mean;			/* mean of x */
	double		sd;				/* sample standard deviation of x */
	double		h;				/* Silverman bandwidth of x */
} RobustProfileBin;

typedef struct RobustProfile
{
	char	   *key;			/* "<alias>" or "<alias>=<alias>" */
	int			npoints;
	double	   *sel_est;		/* sorted by (sel_est, x) */
	double	   *x;				/* log(sel_true / sel_est), same order */
	int			nbins;
	RobustProfileBin bins[ROBUST_MAX_BINS];
} RobustProfile;

extern const RobustProfile *robust_get_profile(const char *key);
extern int	robust_profile_find_bin(const RobustProfile *prof, double sel_est);

#endif							/* ROBUSTPROFILE_H */
