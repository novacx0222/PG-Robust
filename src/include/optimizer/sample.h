//
// Created by Xuan Chen on 2025/9/22.
// Modified by Xuan Chen on 2025/9/24.
// Modified by Xuan Chen on 2025/10/2.
//

#ifndef SAMPLE_H
#define SAMPLE_H

#include "postgres.h"
#include "nodes/pathnodes.h"

#define DIST_MAX_SAMPLE   100

/* GUC Parameters */
extern bool enable_rows_dist;
extern int error_sample_count;
extern int error_sample_seed;

/* Forward Declarations (Sample itself is declared in nodes/pathnodes.h) */
typedef struct ErrorProfileRaw ErrorProfileRaw;

typedef struct ErrorSampleParams ErrorSampleParams;

typedef struct ErrorProfile ErrorProfile;

struct Sample {
    int sample_count;
    double sample[DIST_MAX_SAMPLE];
};

/* ------------------------------- Utilities ------------------------------- */
double clamp01(
    double sel
);

void calc_mean_std(
    const double *array,
    int n_samples,
    double *res_mean,
    double *res_std
);

/* ------------------------------- Samples ------------------------------- */
Sample *initialize_sample(
    int sample_count
);

Sample *duplicate_sample(
    const Sample *src_sample
);

Sample *make_sample_by_single_value(
    double val
);

Sample *make_sample_by_scale_factor(
    const Sample *src_sample,
    double factor
);

Sample *make_partial_rows_sample(
    const Sample *src_sample,
    double parallel_divisor
);

void clamp_rows_sample(
    Sample *rows_sample
);

void robust_fill_path_samples(
    PlannerInfo *root,
    Path *path
);

/* A base relation's rows saved by robust_project_base_rows() */
typedef struct RobustSavedRows {
    RelOptInfo *rel;
    double rows;
} RobustSavedRows;

List *robust_project_base_rows(
    PlannerInfo *root,
    Relids relids,
    int s
);

void robust_restore_base_rows(
    List *saved
);

/* ------------------------------- Relations ------------------------------- */
void set_baserel_rows_sample(
    const PlannerInfo *root,
    RelOptInfo *baserel,
    double sel_est
);

void set_joinrel_rows_sample(
    PlannerInfo *root,
    RelOptInfo *joinrel,
    RelOptInfo *outer_rel,
    RelOptInfo *inner_rel,
    SpecialJoinInfo *sjinfo,
    List *restrictlist
);

#endif // SAMPLE_H
