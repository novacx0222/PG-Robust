/*-------------------------------------------------------------------------
 *
 * robustsample.c
 *	  Drawing estimation errors from RobDP error profiles.
 *
 * A querylet instance gets N = error_sample_count draws
 *
 *		x_s = log(sel_true / sel_est),	s = 0 .. N-1,
 *
 * from the bin of its profile that its estimate falls in (see
 * robustprofile.c).  Draw s picks one of the bin's points uniformly at
 * random and, when a bandwidth h > 0 is in effect, adds h * Z with Z
 * standard normal: a Gaussian kernel density estimate of the bin.  The
 * bandwidth is error_sample_kde_bandwidth if positive, the bin's Silverman
 * bandwidth if negative, and 0 (a plain bootstrap: every x_s is one of the
 * bin's log ratios) if zero.
 *
 * The random numbers come from two pg_prng (xoroshiro128**) streams, one
 * for the points and one for the kernel noise, seeded by
 *
 *		seed  = hash_combine64(hash_bytes_extended(key, error_sample_seed),
 *							   hash_bytes_extended(inst, 0))
 *		noise = hash_combine64(seed, ROBUST_NOISE_STREAM)
 *
 * where key is the profile's name and inst names the querylet instance (the
 * alias, or "a=b" for the join clauses between a and b; see S4/S5).  So:
 *
 *	- the draws are a function of (error_sample_seed, key, inst, bin, N,
 *	  bandwidth) only: the same in every pass, every planning and every
 *	  session;
 *	- two instances of the same profile, such as the two sides of a
 *	  self-join, get independent draws;
 *	- the draws for N are the first N of the draws for any larger N, since
 *	  draw s only depends on the streams before it;
 *	- the bandwidth does not change which points are drawn, only the noise
 *	  added to them.
 *
 * Unlike v20, the bin is not part of the seed.  Two estimates of the same
 * instance that fall in different bins therefore use the same random
 * numbers (common random numbers), and only the points they select differ.
 *
 * The point plus the noise is computed in separate statements so that the
 * compiler cannot contract it into a fused multiply-add; the harness
 * reproduces the draws bit for bit in Python.
 *
 * src/backend/optimizer/util/robustsample.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "optimizer/robustprofile.h"

/* Seeds the kernel-noise stream from the instance's seed ("noise") */
#define ROBUST_NOISE_STREAM		UINT64CONST(0x6e6f697365)

/*
 * The seed of the draws of querylet instance "inst" from profile "key".
 */
uint64
robust_draw_seed(const char *key, const char *inst)
{
	uint64		seed;

	seed = hash_bytes_extended((const unsigned char *) key, (int) strlen(key),
							   (uint64) error_sample_seed);
	return hash_combine64(seed,
						  hash_bytes_extended((const unsigned char *) inst,
											  (int) strlen(inst), 0));
}

/*
 * The KDE bandwidth used for bin b of prof, by error_sample_kde_bandwidth.
 */
double
robust_draw_bandwidth(const RobustProfile *prof, int b)
{
	if (error_sample_kde_bandwidth > 0.0)
		return error_sample_kde_bandwidth;
	if (error_sample_kde_bandwidth < 0.0)
		return prof->bins[b].h;
	return 0.0;
}

/*
 * robust_draw_errors
 *		Draw error_sample_count errors for querylet instance "inst", whose
 *		selectivity estimate is sel_est, from profile prof.
 *
 * Returns a vector of length error_sample_count, and the bin used in *bin
 * if bin is not NULL.  Returns NULL if the profile has no non-empty bin
 * (robustprofile.c never returns such a profile; this is a safety net).
 */
RobustVec *
robust_draw_errors(const RobustProfile *prof, const char *inst,
				   double sel_est, int *bin)
{
	int			b;
	int			first;
	int			n;
	double		h;
	uint64		seed;
	pg_prng_state rng;
	pg_prng_state noise_rng;
	RobustVec  *x;

	Assert(prof != NULL && inst != NULL);

	b = robust_profile_find_bin(prof, sel_est);
	if (bin != NULL)
		*bin = b;
	if (b < 0 || prof->bins[b].n <= 0)
		return NULL;

	first = prof->bins[b].first;
	n = prof->bins[b].n;
	h = robust_draw_bandwidth(prof, b);
	seed = robust_draw_seed(prof->key, inst);

	pg_prng_seed(&rng, seed);
	pg_prng_seed(&noise_rng, hash_combine64(seed, ROBUST_NOISE_STREAM));
	x = robust_vec_make(error_sample_count);
	for (int s = 0; s < error_sample_count; s++)
	{
		int			idx = first + (int) pg_prng_uint64_range(&rng, 0, (uint64) (n - 1));
		double		v = prof->x[idx];

		if (h > 0.0)
		{
			double		z = pg_prng_double_normal(&noise_rng);
			double		noise = h * z;

			v = v + noise;
		}
		x->v[s] = v;
	}

	if (robust_debug_dump)
	{
		RobustDump	d;
		char		hex[17];

		snprintf(hex, sizeof(hex), "%016" INT64_MODIFIER "x", seed);
		robust_dump_begin(&d, "draw");
		robust_dump_str(&d, "key", prof->key);
		robust_dump_str(&d, "inst", inst);
		robust_dump_str(&d, "seed", hex);
		robust_dump_double(&d, "sel_est", sel_est);
		robust_dump_int(&d, "bin", b);
		robust_dump_double(&d, "h", h);
		robust_dump_vec(&d, "x", x);
		robust_dump_end(&d);
	}
	return x;
}
