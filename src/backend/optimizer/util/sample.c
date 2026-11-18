//
// Created by Xuan Chen on 2025/9/22.
// Modified by Xuan Chen on 2025/9/24.
// Modified by Xuan Chen on 2025/10/2.
//

#include "postgres.h"

#include "optimizer/sample.h"
#include "optimizer/cost_check.h"
#include "optimizer/cost_ext.h"
#include "optimizer/kde.h"
#include "optimizer/ep.h"
#include "optimizer/optimizer.h"
#include "utils/smem.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* The per-sample arrays of JoinCostWorkspace must be able to hold a Sample */
StaticAssertDecl(JCW_MAX_SAMPLE == DIST_MAX_SAMPLE,
                 "JCW_MAX_SAMPLE must equal DIST_MAX_SAMPLE");

/* GUC Parameters */
bool enable_rows_dist;
int error_sample_count;
int error_sample_seed;

/* ------------------------------- Utilities ------------------------------- */
double clamp01(const double sel) {
    if (sel < 0.0) {
        return 0.0;
    }
    if (sel > 1.0) {
        return 1.0;
    }
    return sel;
}

void calc_mean_std(
    const double *array,
    const int n_samples,
    double *res_mean,
    double *res_std
) {
    if (n_samples <= 0) {
        *res_mean = 0.0;
        *res_std = 0.0;
        return;
    }
    double sum = 0.0;
    for (int i = 0; i < n_samples; ++i) {
        sum += array[i];
    }
    double sum_of_squares = 0.0;
    const double mean = sum / n_samples;
    /* RobDP P1 (O4): v20 never stored the mean */
    *res_mean = mean;
    for (int i = 0; i < n_samples; ++i) {
        const double diff = mean - array[i];
        sum_of_squares += diff * diff;
    }
    if (n_samples == 1) {
        *res_std = 0.0;
    } else {
        *res_std = sqrt(sum_of_squares / (n_samples - 1));
    }
}

/* ------------------------------- Samples ------------------------------- */
Sample *initialize_sample(
    const int sample_count
) {
    Sample *dst_sample = palloc0(sizeof(Sample));
    dst_sample->sample_count = sample_count;
    return dst_sample;
}

Sample *duplicate_sample(
    const Sample *src_sample
) {
    /*
     * RobDP P0: a missing source vector stays missing; callers that need a
     * vector get one from robust_fill_path_samples().  (v20 crashed here.)
     */
    if (src_sample == NULL)
        return NULL;

    Sample *dst_sample = palloc0(sizeof(Sample));
    memcpy(dst_sample, src_sample, sizeof(Sample));
    return dst_sample;
}

Sample *make_sample_by_single_value(
    const double val
) {
    Sample *dst_sample = palloc0(sizeof(Sample));
    dst_sample->sample_count = 1;
    dst_sample->sample[0] = val;
    return dst_sample;
}

Sample *make_sample_by_scale_factor(
    const Sample *src_sample,
    const double factor
) {
    Assert(src_sample != NULL);
    Sample *dst_sample = palloc0(sizeof(Sample));
    Assert(dst_sample != NULL);
    dst_sample->sample_count = src_sample->sample_count;
    for (int i = 0; i < dst_sample->sample_count; ++i) {
        dst_sample->sample[i] = src_sample->sample[i] * factor;
    }
    return dst_sample;
}

/*
 * make_partial_rows_sample
 *	  Per-worker row counts of a partial path: clamp_row_est(rows / divisor)
 *	  for every component.
 *
 * This is what the cost functions do to the scalar rows of a partial path
 * (e.g. "path->rows = clamp_row_est(path->rows / parallel_divisor)" in
 * final_cost_nestloop), applied sample by sample (RobDP P1, plan item C7;
 * v20 scaled the vector without clamping, so per-worker row counts could
 * drop below 1 and differ from the 1p functions).
 */
Sample *
make_partial_rows_sample(const Sample *src_sample, double parallel_divisor)
{
    Sample     *dst_sample;

    Assert(src_sample != NULL);
    Assert(parallel_divisor > 0);
    dst_sample = palloc0(sizeof(Sample));
    dst_sample->sample_count = src_sample->sample_count;
    for (int i = 0; i < dst_sample->sample_count; ++i)
        dst_sample->sample[i] = clamp_row_est(src_sample->sample[i] / parallel_divisor);
    return dst_sample;
}

/*
 * clamp_rows_sample
 *	  Apply clamp_row_est() to every component of a row-count vector, in
 *	  place.
 *
 * PostgreSQL clamps every row estimate (at least one row, rounded to an
 * integer), and the cost functions rely on that.  The per-sample row counts
 * must follow the same rule, or sample s would not cost the plan the way
 * PostgreSQL would if the true cardinalities were those of sample s (plan
 * invariant C5, item R5).
 */
void
clamp_rows_sample(Sample *rows_sample)
{
    if (rows_sample == NULL)
        return;
    for (int i = 0; i < rows_sample->sample_count; ++i)
        rows_sample->sample[i] = clamp_row_est(rows_sample->sample[i]);
}

/*
 * robust_fill_path_samples
 *	  Make sure a path has its sample vectors.
 *
 * Only the path types with *_2p cost functions get their vectors while they
 * are costed.  Every other path leaves them NULL, and the vector cost code
 * and the path selection strategies dereference them unconditionally, so
 * before such a path is added to a pathlist or used as a join or gather
 * input we attach them here:
 *
 * - During the vector passes (0 and 2), for paths of base and join
 *	 relations, robust_derive_path_samples() (nodecost_ext.c) derives real
 *	 vectors for the node types whose cost is a known function of their
 *	 inputs: Material, Memoize, Sort, Append, bitmap heap scans, ...  (RobDP
 *	 P1; v20 and P0 gave all of them constants.)
 *
 * - Otherwise we attach length-1 vectors holding the path's scalar rows /
 *	 startup_cost / total_cost.  Reading a length-1 vector gives the same
 *	 value as the scalar fallback in GET_ROW / GET_COST, so this changes no
 *	 cost; such paths are treated as having no uncertainty.  The 1p
 *	 functions of pass 1 and the upper planning of pass 3 never read the
 *	 vectors, so constants are all they need.
 *
 * "root" may be NULL (dummy relations are marked without one); constants
 * are attached then.  robust_debug_check_costs counts the paths that got
 * constants during the vector passes.
 */
void
robust_fill_path_samples(PlannerInfo *root, Path *path)
{
    bool        filled = false;

    if (path == NULL)
        return;

    if (path->rows_sample != NULL && path->startup_cost_sample != NULL &&
        path->total_cost_sample != NULL)
        return;

    if (root != NULL && (root->pass == 0 || root->pass == 2) &&
        path->parent != NULL && !IS_UPPER_REL(path->parent) &&
        robust_derive_path_samples(root, path))
    {
        Assert(path->rows_sample->sample_count == 1 ||
               path->rows_sample->sample_count == error_sample_count);
        return;
    }

    if (path->rows_sample == NULL)
    {
        path->rows_sample = make_sample_by_single_value(path->rows);
        filled = true;
    }
    if (path->startup_cost_sample == NULL)
    {
        path->startup_cost_sample = make_sample_by_single_value(path->startup_cost);
        filled = true;
    }
    if (path->total_cost_sample == NULL)
    {
        path->total_cost_sample = make_sample_by_single_value(path->total_cost);
        filled = true;
    }

    if (filled)
    {
        elog(DEBUG2, "RobDP: constant sample vectors attached to path type %d",
             (int) path->pathtype);
        robust_check_note_fill(root, path);
    }

    /* T5: vectors are either constants or full-length */
    Assert(path->rows_sample->sample_count == 1 ||
           path->rows_sample->sample_count == error_sample_count);
    Assert(path->total_cost_sample->sample_count == 1 ||
           path->total_cost_sample->sample_count == error_sample_count);
}

/*
 * robust_project_base_rows
 *		Set rows of the base relations among "relids" to their sample-s
 *		component; returns what robust_restore_base_rows() needs to put the
 *		old values back.
 *
 * Some cost functions read a base relation's rows not from their inputs but
 * through the selectivity code: cost_memoize_rescan's estimate_num_groups()
 * of the cache keys scales each Var's ndistinct by its relation's
 * rows / tuples.  In pass-1 round s the base relations hold their s-th rows
 * (the round view), so the per-sample cost at s, and the checker's 1p
 * recomputation of it, must see the same rows.  Relations without a varying
 * vector, and relids that are not base relations (outer joins), are left
 * alone.
 */
List *
robust_project_base_rows(PlannerInfo *root, Relids relids, int s)
{
    List       *saved = NIL;
    int         relid = -1;

    while ((relid = bms_next_member(relids, relid)) >= 0)
    {
        RelOptInfo *rel;
        RobustSavedRows *item;

        if (relid >= root->simple_rel_array_size)
            continue;
        rel = root->simple_rel_array[relid];
        if (rel == NULL || rel->rows_sample == NULL ||
            rel->rows_sample->sample_count <= 1)
            continue;
        item = palloc(sizeof(RobustSavedRows));
        item->rel = rel;
        item->rows = rel->rows;
        saved = lappend(saved, item);
        rel->rows = rel->rows_sample->sample[s];
    }
    return saved;
}

void
robust_restore_base_rows(List *saved)
{
    ListCell   *lc;

    foreach(lc, saved)
    {
        RobustSavedRows *item = (RobustSavedRows *) lfirst(lc);

        item->rel->rows = item->rows;
    }
    list_free_deep(saved);
}

/* ------------------------------- Relations ------------------------------- */

/*
 * The relations' row-count vectors (RobDP P1, plan items R1-R6).
 *
 * Sample s is a scenario: the true selectivity of every querylet (a base
 * relation's restrictions, or the join clauses between two relations) is
 * PostgreSQL's estimate times exp(x_s), where x_s is a draw from the
 * querylet's error profile (draw_error_sample).  A relation's rows[s] is
 * what PostgreSQL would estimate in that scenario: the stock formula applied
 * to the input relations' rows[s] and the scaled selectivity.
 *
 *	- Base relation: rows[s] = clamp_row_est(tuples * min(1, sel * exp(x_s))).
 *	- Join relation: calc_joinrel_rows() with the inputs' rows[s] and the
 *	  join's own selectivity multiplied by exp(sum of x_s over the querylets
 *	  of its join clauses), so jointypes, foreign keys and pushed-down quals
 *	  are handled as PostgreSQL handles them (R2; v20 used
 *	  outer * inner * sel for every jointype).
 *	- A querylet without a profile has x_s = 0.  A join without profiles
 *	  still gets the per-sample product of its inputs (R3; v20 collapsed it
 *	  to a constant, dropping the uncertainty of everything below), and a
 *	  relation none of whose inputs vary keeps PostgreSQL's estimate exactly
 *	  (R1).
 *
 * rel->rows is the clamped mean of the vector (v20's convention), or the
 * round's own component during pass 1.  Every component is clamped like the
 * scalar estimate (R5).
 */

/*
 * The relation's row-count vector, or a constant one if it has none.
 *
 * Append relations (inheritance / partitioned parents, flattened UNION ALL)
 * get their rows from set_append_rel_size(), which does not build a vector.
 */
static const Sample *
rel_rows_sample(const RelOptInfo *rel)
{
    if (rel->rows_sample != NULL)
        return rel->rows_sample;
    return make_sample_by_single_value(rel->rows);
}

/* Component i of a vector of length 1 or N */
static inline double
sample_at(const Sample *v, int i)
{
    return (v->sample_count == 1) ? v->sample[0] : v->sample[i];
}

/* Set rel->rows from the relation's vector, as described above */
static void
set_rows_from_sample(const PlannerInfo *root, RelOptInfo *rel)
{
    const Sample *v = rel->rows_sample;
    double      mean = 0.0;

    if (v->sample_count > 1 && root->pass == 1)
    {
        rel->rows = v->sample[root->round];
        return;
    }
    for (int i = 0; i < v->sample_count; ++i)
        mean += v->sample[i];
    rel->rows = clamp_row_est(mean / (double) v->sample_count);
}

void set_baserel_rows_sample(
    const PlannerInfo *root,
    RelOptInfo *baserel,
    const double sel_est
) {
    /* 1. Resolve relation aliases (original alias and a standard fallback). */
    const char *alias = get_alias(root, baserel->relid);
    const char *alias_fallback = get_std_alias(root, baserel->relid);

    /* 2. Look up the profile and draw this relation's error samples. */
    ErrorProfile *ep;
    const char *profile_key;
    Sample      log_error;

    if (!get_error_profile(alias, alias_fallback, &ep, &profile_key) ||
        !draw_error_sample(ep, profile_key, alias, sel_est, &log_error))
    {
        /*
         * No profile, or nothing in the relevant bin: keep PostgreSQL's
         * estimate (R1; it is the clamped tuples * sel_est already).
         */
        baserel->rows_sample = make_sample_by_single_value(baserel->rows);
        return;
    }

    /*
     * 3. rows[s] = tuples * true selectivity of sample s.  A selectivity
     * above 1 is not a scenario PostgreSQL could estimate, so it is clamped
     * like every selectivity estimate (v20 did not clamp it).
     */
    Sample *rows_sample = initialize_sample(log_error.sample_count);
    for (int i = 0; i < log_error.sample_count; ++i)
        rows_sample->sample[i] =
            baserel->tuples * clamp01(sel_est * exp(log_error.sample[i]));
    clamp_rows_sample(rows_sample);

    baserel->rows_sample = rows_sample;
    set_rows_from_sample(root, baserel);
}

/*
 * One querylet of a join: the Var = Var join clauses between one relation
 * of the outer side and one of the inner side.
 */
typedef struct JoinQuerylet
{
    char        alias[SM_KEY_LEN];          /* "a=b", canonical order */
    char        alias_fallback[SM_KEY_LEN]; /* same with standard aliases */
    List       *clauses;
} JoinQuerylet;

/*
 * make_join_log_error
 *		The summed log-error samples of the querylets among a join's
 *		clauses, or NULL if none of them has a profile.
 *
 * The join clauses are grouped by the pair of relations their two Vars
 * belong to, one querylet per pair (R4: v20 took the profile of the first
 * Var = Var clause only and applied it to the selectivity of all clauses).
 * Each querylet's selectivity estimate picks the bin of its profile, and its
 * samples are seeded by its own aliases, so the two instances of a
 * self-join get independent errors.
 *
 * Like PostgreSQL's row estimate, the vector depends on which pair of input
 * relations builds the join relation first: it is computed once, from that
 * pair's restrictlist, and reused (see set_joinrel_rows_sample).  With
 * equivalence classes, PostgreSQL generates the join clauses a given pair
 * needs, so the querylets of a join are those of the clauses it applies.
 */
static Sample *
make_join_log_error(PlannerInfo *root,
                    const RelOptInfo *outer_rel,
                    const RelOptInfo *inner_rel,
                    List *restrictlist,
                    SpecialJoinInfo *sjinfo)
{
    List       *querylets = NIL;
    Sample     *sum = NULL;
    ListCell   *lc;

    foreach(lc, restrictlist)
    {
        RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
        const OpExpr *opexpr;
        const Var  *leftvar;
        const Var  *rightvar;
        char        alias[SM_KEY_LEN];
        char        alias_fallback[SM_KEY_LEN];
        JoinQuerylet *q = NULL;
        ListCell   *lq;

        /* Both sides must be Vars (as in v20, RelabelType etc. are skipped) */
        if (!IsA(rinfo->clause, OpExpr))
            continue;
        opexpr = (const OpExpr *) rinfo->clause;
        if (list_length(opexpr->args) != 2 ||
            !IsA(linitial(opexpr->args), Var) ||
            !IsA(lsecond(opexpr->args), Var))
            continue;
        leftvar = (const Var *) linitial(opexpr->args);
        rightvar = (const Var *) lsecond(opexpr->args);

        /* Accept either orientation: (outer,left)-(inner,right) or swapped. */
        if (!((bms_is_member(leftvar->varno, outer_rel->relids) &&
               bms_is_member(rightvar->varno, inner_rel->relids)) ||
              (bms_is_member(rightvar->varno, outer_rel->relids) &&
               bms_is_member(leftvar->varno, inner_rel->relids))))
            continue;

        {
            const char *l = get_alias(root, leftvar->varno);
            const char *r = get_alias(root, rightvar->varno);
            const char *lstd = get_std_alias(root, leftvar->varno);
            const char *rstd = get_std_alias(root, rightvar->varno);

            /* Canonicalize order to avoid duplicate "A=B" vs "B=A". */
            if (strcmp(l, r) < 0)
                snprintf(alias, sizeof(alias), "%s=%s", l, r);
            else
                snprintf(alias, sizeof(alias), "%s=%s", r, l);
            if (strcmp(lstd, rstd) < 0)
                snprintf(alias_fallback, sizeof(alias_fallback), "%s=%s", lstd, rstd);
            else
                snprintf(alias_fallback, sizeof(alias_fallback), "%s=%s", rstd, lstd);
        }

        foreach(lq, querylets)
        {
            JoinQuerylet *cand = (JoinQuerylet *) lfirst(lq);

            if (strcmp(cand->alias, alias) == 0)
            {
                q = cand;
                break;
            }
        }
        if (q == NULL)
        {
            q = palloc0(sizeof(JoinQuerylet));
            strlcpy(q->alias, alias, sizeof(q->alias));
            strlcpy(q->alias_fallback, alias_fallback, sizeof(q->alias_fallback));
            querylets = lappend(querylets, q);
        }
        q->clauses = lappend(q->clauses, rinfo);
    }

    foreach(lc, querylets)
    {
        JoinQuerylet *q = (JoinQuerylet *) lfirst(lc);
        ErrorProfile *ep;
        const char *profile_key;
        Selectivity sel_est;
        Sample      log_error;

        if (!get_error_profile(q->alias, q->alias_fallback, &ep, &profile_key))
            continue;
        /* the querylet's own estimate, as v20 computed it for all clauses */
        sel_est = clauselist_selectivity(root, q->clauses, 0,
                                         sjinfo->jointype, sjinfo);
        if (!draw_error_sample(ep, profile_key, q->alias, sel_est, &log_error))
            continue;
        if (sum == NULL)
            sum = initialize_sample(log_error.sample_count);
        for (int i = 0; i < log_error.sample_count; ++i)
            sum->sample[i] += log_error.sample[i];
    }
    return sum;
}

/*
 * Join row-count vectors computed so far, by relids (R6).
 *
 * Pass 1 rebuilds every join relation once per round and pass 2 once more;
 * v20 recomputed the vector (and its selectivities) every time.  It does
 * not depend on the round, so it is computed when a join relation is first
 * built and looked up afterwards.  The table lives in the planner's context,
 * like the caches PostgreSQL keeps for GEQO, which builds join relations in
 * short-lived contexts too.
 */
typedef struct JoinRowsEntry
{
    Relids      relids;         /* hash key --- MUST BE FIRST */
    Sample     *rows_sample;
} JoinRowsEntry;

static HTAB *
join_rows_hash(PlannerInfo *root)
{
    if (root->robust_join_rows_hash == NULL)
    {
        HASHCTL     hash_ctl;

        hash_ctl.keysize = sizeof(Relids);
        hash_ctl.entrysize = sizeof(JoinRowsEntry);
        hash_ctl.hash = bitmap_hash;
        hash_ctl.match = bitmap_match;
        hash_ctl.hcxt = root->planner_cxt ? root->planner_cxt : CurrentMemoryContext;
        root->robust_join_rows_hash =
            hash_create("RobDP join rows samples", 256, &hash_ctl,
                        HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);
    }
    return root->robust_join_rows_hash;
}

void set_joinrel_rows_sample(
    PlannerInfo *root,
    RelOptInfo *joinrel,
    RelOptInfo *outer_rel,
    RelOptInfo *inner_rel,
    SpecialJoinInfo *sjinfo,
    List *restrictlist
) {
    HTAB       *hash = join_rows_hash(root);
    JoinRowsEntry *entry;
    bool        found;

    entry = (JoinRowsEntry *) hash_search(hash, &joinrel->relids, HASH_FIND, NULL);
    if (entry == NULL)
    {
        JoinSizeSelectivities sel;
        const Sample *outer_rows = rel_rows_sample(outer_rel);
        const Sample *inner_rows = rel_rows_sample(inner_rel);
        Sample     *log_error;
        Sample     *rows_sample;
        int         n;
        MemoryContext oldcxt;

        calc_joinrel_selectivities(root, joinrel, outer_rel, inner_rel,
                                   sjinfo, restrictlist, &sel);
        log_error = make_join_log_error(root, outer_rel, inner_rel,
                                        restrictlist, sjinfo);

        n = (outer_rows->sample_count > 1 || inner_rows->sample_count > 1 ||
             log_error != NULL) ? error_sample_count : 1;

        /* the entry and its vector must outlive the current context */
        oldcxt = MemoryContextSwitchTo(root->planner_cxt ? root->planner_cxt
                                       : CurrentMemoryContext);
        rows_sample = initialize_sample(n);
        for (int i = 0; i < n; ++i)
            rows_sample->sample[i] =
                calc_joinrel_rows(&sel, sample_at(outer_rows, i),
                                  sample_at(inner_rows, i),
                                  log_error ? exp(log_error->sample[i]) : 1.0);
        entry = (JoinRowsEntry *) hash_search(hash, &joinrel->relids,
                                              HASH_ENTER, &found);
        Assert(!found);
        entry->relids = bms_copy(joinrel->relids);
        entry->rows_sample = rows_sample;
        MemoryContextSwitchTo(oldcxt);
    }

    /* a private copy: the path code treats rel->rows_sample as its own */
    joinrel->rows_sample = duplicate_sample(entry->rows_sample);
    set_rows_from_sample(root, joinrel);
}
