//
// Created by Xuan Chen on 2025/10/2.
//

#ifndef EP_H
#define EP_H

#include "postgres.h"
#include "nodes/pathnodes.h"
#include "optimizer/sample.h"

/*
 * Points read per profile (RobDP: was 1024; the regenerated PAR2QO profiles
 * have up to 50 x 50 = 2500, and read_error_profile used to drop the rest
 * silently).  ErrorProfile is about 24 bytes per point, cached per backend.
 */
#define EP_MAX_SAMPLE     4096
#define EP_MAX_BIN        8

/* Forward Declarations */
typedef struct Sample Sample;

typedef struct ErrorProfileRaw ErrorProfileRaw;

typedef struct ErrorSampleParams ErrorSampleParams;

typedef struct ErrorProfile ErrorProfile;

struct ErrorProfileRaw {
    double sel_true;
    double sel_est;
};

/*
 * Parameters of one bin of an error profile.  The points of bin b are
 * data[first .. first + n_points - 1] (data is sorted by sel_est).
 */
struct ErrorSampleParams {
    int bin_index;
    int first; /* index of the bin's first point in data[] */
    int n_points; /* number of raw samples in this bin */
    double bandwidth_h; /* Silverman's rule-of-thumb KDE bandwidth */
    double mean_logratio; /* mean of log(sel_true/sel_est) */
    double std_logratio; /* std of log(sel_true/sel_est) */
    double sel_est_lo; /* left boundary of sel_est */
    double sel_est_hi; /* right boundary of sel_est */
};

/*
 * An error profile: the raw (sel_true, sel_est) pairs of one querylet,
 * binned by sel_est.  (RobDP P1: v20 also stored error_sample_count samples
 * per bin, drawn once when the profile was loaded with a seed that depended
 * on the bin only.  Samples are now drawn when a relation uses the profile,
 * see draw_error_sample(), so they depend on the relation instance and on
 * the current GUC values.)
 */
struct ErrorProfile {
    int sample_count; /* number of raw points */
    double std_true;
    double std_est;
    int bin_count; /* error_bin_count the bins were built for, 0 if none */
    double error_sample_thresh[EP_MAX_BIN]; /* sel_est threshold of each bin */
    ErrorSampleParams params[EP_MAX_BIN]; /* parameters of each bin */
    ErrorProfileRaw data[EP_MAX_SAMPLE]; /* input raw data */
    double log_ratio[EP_MAX_SAMPLE]; /* log(sel_true/sel_est) of data[i] */
};


/* ------------------------------- Aliases ------------------------------- */
char *get_alias(
    const PlannerInfo *root,
    Index relid
);

char *get_std_alias(
    const PlannerInfo *root,
    Index relid
);

/* ------------------------------- Error Profiles ------------------------------- */
int read_error_profile(
    const char *filename,
    ErrorProfile *ep
);

bool get_error_profile(
    const char *alias,
    const char *alias_fallback,
    ErrorProfile **ep,
    const char **profile_key
);

#endif // EP_H
