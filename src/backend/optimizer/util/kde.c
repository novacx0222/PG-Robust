//
// Created by Xuan Chen on 2025/9/22.
// Modified by Xuan Chen on 2025/10/2.
//
// RobDP P1: binning and sampling reworked (plan items K1, K2, K4):
//   - samples are drawn when a relation uses a profile, seeded by the profile,
//     the relation instance and the bin (draw_error_sample);
//   - the RNG is pg_prng instead of an LCG reduced modulo 10^6;
//   - Silverman's rule-of-thumb bandwidth can be selected;
//   - the bins remember the error_bin_count they were built for, so that a
//     change of the GUC rebuilds them (see SessionMemEnsureLoaded).
//

#include "postgres.h"

#include <math.h>

#include "common/hashfn.h"
#include "common/pg_prng.h"
#include "optimizer/sample.h"
#include "optimizer/ep.h"
#include "optimizer/kde.h"

static const double EPS = 1e-12;

/* GUC Parameters */
int error_bin_count = 1;
double error_sample_kde_bandwidth = 0.0;

/* ---------------- Utilities ---------------- */

/* Comparator for sorting by sel_est ascending */
static int cmp_sel_est_asc(const void *a, const void *b) {
    const ErrorProfileRaw *pa = a;
    const ErrorProfileRaw *pb = b;
    if (pa->sel_est < pb->sel_est) return -1;
    if (pa->sel_est > pb->sel_est) return 1;
    return 0;
}

static int cmp_double_asc(const void *a, const void *b) {
    const double da = *(const double *) a;
    const double db = *(const double *) b;
    if (da < db) return -1;
    if (da > db) return 1;
    return 0;
}

/* Safe log ratio: log(max(sel_true,EPS)/max(sel_est,EPS)) */
static double safe_log_ratio(const double sel_true, const double sel_est) {
    const double t = fmax(sel_true, EPS);
    const double e = fmax(sel_est, EPS);
    return log(t / e);
}

/*
 * Silverman's rule of thumb for a Gaussian KDE of x[0..n-1]:
 *
 *		h = 0.9 * min(sd, IQR / 1.34) * n^(-1/5)
 *
 * falling back to sd when the interquartile range is 0 (many equal points),
 * and to 0 (plain bootstrap) when all points are equal.
 */
static double silverman_bandwidth(const double *x, const int n, const double sd) {
    if (n < 2 || sd <= 0.0) {
        return 0.0;
    }
    double *sorted = palloc(sizeof(double) * n);
    memcpy(sorted, x, sizeof(double) * n);
    qsort(sorted, (size_t) n, sizeof(double), cmp_double_asc);
    const double iqr = sorted[(3 * (n - 1)) / 4] - sorted[(n - 1) / 4];
    pfree(sorted);

    double spread = sd;
    if (iqr > 0.0 && iqr / 1.34 < sd) {
        spread = iqr / 1.34;
    }
    return 0.9 * spread * pow((double) n, -0.2);
}

/* ---------------- Build thresholds + params ---------------- */

/*
 * Bin the points of a profile by sel_est (equal-count bins) and record each
 * bin's boundaries and statistics.  Sorts ep->data and fills
 * ep->log_ratio.  No samples are drawn here; see draw_error_sample().
 */
void make_error_bins(ErrorProfile *ep) {
    Assert(ep != NULL);
    Assert(error_bin_count >= 1 && error_bin_count <= EP_MAX_BIN);

    ep->bin_count = error_bin_count;
    if (ep->sample_count <= 0) {
        for (int b = 0; b < EP_MAX_BIN; ++b) {
            ep->error_sample_thresh[b] = NAN;
            ep->params[b].bin_index = -1;
            ep->params[b].first = 0;
            ep->params[b].n_points = 0;
        }
        return;
    }

    /* 1) sort by sel_est; the log ratios follow the sorted order */
    qsort(ep->data, (size_t) ep->sample_count, sizeof(ErrorProfileRaw), cmp_sel_est_asc);
    for (int i = 0; i < ep->sample_count; ++i) {
        ep->log_ratio[i] = safe_log_ratio(ep->data[i].sel_true, ep->data[i].sel_est);
    }

    const int N = ep->sample_count;
    int start = 0;
    double prev_hi = -INFINITY;

    for (int b = 0; b < error_bin_count; ++b) {
        /* 2) equal-count binning on sel_est */
        int end = (int) llround(((long long) (b + 1) * (long long) N) / (double) error_bin_count);
        if (end > N) {
            end = N;
        }
        if (b == error_bin_count - 1) {
            end = N;
        }
        const int count = end - start;

        const double sel_lo = (b == 0) ? -INFINITY : ep->error_sample_thresh[b - 1];
        const double sel_hi = (count > 0) ? ep->data[end - 1].sel_est : ((b == 0) ? INFINITY : prev_hi);

        ep->error_sample_thresh[b] = sel_hi;
        prev_hi = sel_hi;

        /* 3) statistics of x = log(sel_true/sel_est) over this bin */
        double mean_x = 0.0, std_x = 0.0;
        if (count > 0) {
            calc_mean_std(&ep->log_ratio[start], count, &mean_x, &std_x);
        }

        /* 4) record params */
        ep->params[b].bin_index = b;
        ep->params[b].first = start;
        ep->params[b].n_points = count;
        ep->params[b].bandwidth_h = silverman_bandwidth(&ep->log_ratio[start], count, std_x);
        ep->params[b].mean_logratio = mean_x;
        ep->params[b].std_logratio = std_x;
        ep->params[b].sel_est_lo = sel_lo;
        ep->params[b].sel_est_hi = sel_hi;

        start = end;
    }

    /* clear remaining bins (if any) */
    for (int b = error_bin_count; b < EP_MAX_BIN; ++b) {
        ep->error_sample_thresh[b] = NAN;
        ep->params[b].bin_index = -1;
        ep->params[b].first = 0;
        ep->params[b].n_points = 0;
        ep->params[b].bandwidth_h = 0.0;
        ep->params[b].mean_logratio = 0.0;
        ep->params[b].std_logratio = 0.0;
        ep->params[b].sel_est_lo = NAN;
        ep->params[b].sel_est_hi = NAN;
    }
}

/* ------------------------------- Sampling ------------------------------- */

/* Optional helper: find bin by sel_est using (thresh[b-1], thresh[b]].
   Returns -1 if none found. */
int find_bin_by_sel_est(const ErrorProfile *ep, const double sel_est) {
    if (ep->bin_count <= 0) {
        return -1;
    }
    for (int b = 0; b < ep->bin_count; ++b) {
        const double lo = (b == 0) ? -INFINITY : ep->error_sample_thresh[b - 1];
        const double hi = ep->error_sample_thresh[b];
        if (sel_est > lo && sel_est <= hi) return b;
    }
    /* fallback: the last non-empty bin */
    for (int b = ep->bin_count - 1; b >= 0; --b) {
        if (ep->params[b].n_points > 0) return b;
    }
    return -1;
}

/*
 * The KDE bandwidth of bin b, by error_sample_kde_bandwidth: a positive
 * value is used as is, 0 means no smoothing (each sample is one of the
 * bin's log ratios, i.e. a bootstrap; v20 used 1e-6, which is the same in
 * effect), and a negative value selects Silverman's rule of thumb.
 */
static double kde_bandwidth(const ErrorProfile *ep, const int b) {
    if (error_sample_kde_bandwidth > 0.0) {
        return error_sample_kde_bandwidth;
    }
    if (error_sample_kde_bandwidth < 0.0) {
        return ep->params[b].bandwidth_h;
    }
    return 0.0;
}

/*
 * draw_error_sample
 *		Draw error_sample_count log-ratio samples x = log(sel_true / sel_est)
 *		for one relation instance from the bin of "ep" that sel_est falls in.
 *
 * The samples come from a Gaussian KDE of the bin's points: pick a point
 * uniformly, add h * N(0, 1).  The random stream is seeded by
 * hash(error_sample_seed, profile_key, instance_key, bin), so that
 *
 *	- different relations (and the two instances of a self-join, whose
 *	  aliases differ) get independent samples, where v20 seeded by the bin
 *	  alone and so gave every relation in the same bin the same stream (K1);
 *	- the same relation gets the same samples whenever it is planned with
 *	  the same GUC values, in every pass and every query;
 *	- changing error_sample_count, error_sample_seed or the bandwidth takes
 *	  effect at the next planning, without reloading the profiles (K2).
 *
 * Returns false, leaving "out" alone, if the profile has no point in the
 * relevant bin.
 */
bool draw_error_sample(const ErrorProfile *ep, const char *profile_key,
                       const char *instance_key, const double sel_est,
                       Sample *out) {
    Assert(ep != NULL && out != NULL);
    Assert(error_sample_count >= 1 && error_sample_count <= DIST_MAX_SAMPLE);

    const int b = find_bin_by_sel_est(ep, sel_est);
    if (b < 0 || ep->params[b].n_points <= 0) {
        return false;
    }
    const int first = ep->params[b].first;
    const int n = ep->params[b].n_points;
    const double h = kde_bandwidth(ep, b);

    uint64 seed = hash_bytes_extended((const unsigned char *) profile_key,
                                      (int) strlen(profile_key),
                                      (uint64) error_sample_seed);
    seed = hash_combine64(seed,
                          hash_bytes_extended((const unsigned char *) instance_key,
                                              (int) strlen(instance_key), 0));
    seed = hash_combine64(seed, (uint64) b);

    pg_prng_state rng;
    pg_prng_seed(&rng, seed);
    for (int k = 0; k < error_sample_count; ++k) {
        const int idx = first + (int) pg_prng_uint64_range(&rng, 0, (uint64) (n - 1));
        double x = ep->log_ratio[idx];
        if (h > 0.0) {
            x += h * pg_prng_double_normal(&rng);
        }
        out->sample[k] = x;
    }
    out->sample_count = error_sample_count;
    return true;
}
