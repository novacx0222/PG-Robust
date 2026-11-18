//
// Created by Xuan Chen on 2025/11/1.
//

#ifndef COST_EXT_H
#define COST_EXT_H

#include "nodes/pathnodes.h"

/*
 * RobDP cost model selection.
 *
 * The costsize.c entry points that have per-sample counterparts (seqscan,
 * index scan, gather, gather merge and the three join methods) ask one of the
 * functions below which implementation to run:
 *
 *	ROBUST_COST_STOCK	the unmodified PostgreSQL code in costsize.c
 *	ROBUST_COST_1P		*_1p: scalar costs at the point estimate of error
 *						sample root->round (pass 1)
 *	ROBUST_COST_2P		*_2p: per-sample cost vectors (pass 0 and pass 2)
 *
 * With enable_rows_dist off everything uses the stock code, so the planner
 * behaves exactly like PostgreSQL.  Otherwise:
 *
 *	pass	scans	joins	gather / gather merge
 *	0		2P		STOCK	2P
 *	1		1P		1P		1P
 *	2		2P		2P		2P
 *	3		STOCK	STOCK	1P
 *
 * Joins see pass 0 only when the join search is not standard_join_search
 * (GEQO or join_search_hook), and scans see pass 3 only for paths built
 * during upper planning; v20 raised "bad pass number" in both cases, and the
 * stock code is used instead.  Scans of a relation without a row-count
 * vector also use the stock code: such relations are built outside
 * set_baserel_size_estimates(), e.g. by plan_cluster_use_sort() for CLUSTER,
 * and v20 crashed on them.
 */
typedef enum RobustCostModel
{
	ROBUST_COST_STOCK,
	ROBUST_COST_1P,
	ROBUST_COST_2P
} RobustCostModel;

extern RobustCostModel robust_scan_cost_model(const PlannerInfo *root,
											  const RelOptInfo *baserel);
extern RobustCostModel robust_join_cost_model(const PlannerInfo *root);
extern RobustCostModel robust_gather_cost_model(const PlannerInfo *root);

#define LOG2(x)  (log(x) / 0.693147180559945)

#define GET_ROW(s, i, scalar, is_const) \
( ((s) == NULL || (s)->sample_count <= 0) \
? (scalar) : ((is_const) ? (s)->sample[0] : (s)->sample[(i)]) )

#define GET_COST(s, i, scalar, is_const) \
( ((s) == NULL || (s)->sample_count <= 0) \
? (scalar) : ((is_const) ? (s)->sample[0] : (s)->sample[(i)]) )

/* ==== ==== ==== ==== ==== ==== SCAN COST HELPERS ==== ==== ==== ==== ==== ==== */

extern void get_restriction_qual_cost(
    PlannerInfo *root,
    const RelOptInfo *baserel,
    const ParamPathInfo *param_info,
    QualCost *qpqual_cost
);

/* ==== ==== ==== ==== ==== ==== SCAN COST MODEL ==== ==== ==== ==== ==== ==== */

extern void cost_seqscan_1p(
    Path *path,
    PlannerInfo *root,
    const RelOptInfo *baserel,
    const ParamPathInfo *param_info
);

extern void cost_seqscan_2p(
    Path *path,
    PlannerInfo *root,
    const RelOptInfo *baserel,
    const ParamPathInfo *param_info
);

extern void cost_index_1p(
    IndexPath *path,
    PlannerInfo *root,
    double loop_count,
    bool partial_path
);

extern void cost_index_2p(
    IndexPath *path,
    PlannerInfo *root,
    double loop_count,
    bool partial_path
);

/* ==== ==== ==== ==== ==== ==== JOIN COST HELPERS ==== ==== ==== ==== ==== ==== */
double get_path_rows_1p(
    const Path *path,
    int round
);

extern double get_parallel_divisor(
    const Path *path
);

extern double approx_tuple_count(
    PlannerInfo *root,
    const JoinPath *path,
    List *quals
);

extern int32 get_expr_width(
    const PlannerInfo *root,
    const Node *expr
);

extern double relation_byte_size(
    double tuples,
    int width
);

extern double page_size(
    double tuples,
    int width
);

extern bool has_indexed_join_quals(
    NestPath *path
);

/*
 * robust_mergejoin_frozen_materialize: -1 normally.  The cost cross check
 * (cost_check.c) sets it to 0 or 1 around a call of final_cost_mergejoin_1p
 * to make that function use the materialize_inner decision of the 2p path
 * being checked instead of deciding for itself: the decision is structural
 * and made once for all samples, so only the costs of that plan shape are
 * comparable.
 */
extern int	robust_mergejoin_frozen_materialize;

/* ==== ==== ==== ==== ==== ==== 1-PASS JOIN COST MODEL ==== ==== ==== ==== ==== ==== */

extern void initial_cost_nestloop_1p(
    PlannerInfo *root,
    JoinCostWorkspace *workspace,
    JoinType jointype,
    const Path *outer_path,
    Path *inner_path,
    const JoinPathExtraData *extra
);

extern void final_cost_nestloop_1p(
    PlannerInfo *root,
    NestPath *path,
    JoinCostWorkspace *workspace,
    const JoinPathExtraData *extra
);

extern void initial_cost_mergejoin_1p(
    PlannerInfo *root,
    JoinCostWorkspace *workspace,
    JoinType jointype,
    List *mergeclauses,
    Path *outer_path,
    Path *inner_path,
    List *outersortkeys,
    List *innersortkeys,
    JoinPathExtraData *extra
);

extern void final_cost_mergejoin_1p(
    PlannerInfo *root,
    MergePath *path,
    JoinCostWorkspace *workspace,
    const JoinPathExtraData *extra
);

extern void initial_cost_hashjoin_1p(
    PlannerInfo *root,
    JoinCostWorkspace *workspace,
    JoinType jointype,
    const List *hashclauses,
    Path *outer_path,
    Path *inner_path,
    JoinPathExtraData *extra,
    bool parallel_hash
);

extern void final_cost_hashjoin_1p(
    PlannerInfo *root,
    HashPath *path,
    JoinCostWorkspace *workspace,
    const JoinPathExtraData *extra
);

/* ==== ==== ==== ==== ==== ==== 2-PASS JOIN COST MODEL ==== ==== ==== ==== ==== ==== */

extern void initial_cost_nestloop_2p(
    PlannerInfo *root,
    JoinCostWorkspace *workspace,
    JoinType jointype,
    const Path *outer_path,
    Path *inner_path,
    const JoinPathExtraData *extra
);

extern void final_cost_nestloop_2p(
    PlannerInfo *root,
    NestPath *path,
    JoinCostWorkspace *workspace,
    const JoinPathExtraData *extra
);

extern void initial_cost_mergejoin_2p(
    PlannerInfo *root,
    JoinCostWorkspace *workspace,
    JoinType jointype,
    List *mergeclauses,
    Path *outer_path,
    Path *inner_path,
    List *outersortkeys,
    List *innersortkeys,
    JoinPathExtraData *extra
);

extern void final_cost_mergejoin_2p(
    PlannerInfo *root,
    MergePath *path,
    JoinCostWorkspace *workspace,
    const JoinPathExtraData *extra
);

extern void initial_cost_hashjoin_2p(
    PlannerInfo *root,
    JoinCostWorkspace *workspace,
    JoinType jointype,
    const List *hashclauses,
    Path *outer_path,
    Path *inner_path,
    JoinPathExtraData *extra,
    bool parallel_hash
);

extern void final_cost_hashjoin_2p(
    PlannerInfo *root,
    HashPath *path,
    JoinCostWorkspace *workspace,
    const JoinPathExtraData *extra
);

/* ==== ==== ==== ==== ==== ==== OTHER COST MODEL ==== ==== ==== ==== ==== ==== */

extern void cost_gather_1p(
    GatherPath *path,
    PlannerInfo *root,
    const RelOptInfo *rel,
    const ParamPathInfo *param_info,
    const double *rows
);

extern void cost_gather_2p(
    GatherPath *path,
    PlannerInfo *root,
    const RelOptInfo *rel,
    const ParamPathInfo *param_info,
    const double *rows
);

extern void cost_gather_merge_1p(
    GatherMergePath *path,
    PlannerInfo *root,
    const RelOptInfo *rel,
    const ParamPathInfo *param_info,
    Cost input_startup_cost,
    Cost input_total_cost,
    const double *rows
);

extern void cost_gather_merge_2p(
    GatherMergePath *path,
    PlannerInfo *root,
    const RelOptInfo *rel,
    const ParamPathInfo *param_info,
    Cost input_startup_cost,
    Cost input_total_cost,
    const double *rows
);

/* ==== ==== ==== ==== ==== ==== DERIVED VECTORS (nodecost_ext.c) ==== ==== ==== ==== ==== ==== */

extern bool robust_derive_path_samples(PlannerInfo *root, Path *path);

/*
 * Join size estimation, split out of costsize.c's calc_joinrel_size_estimate
 * so that the per-sample join sizes (set_joinrel_rows_sample in sample.c)
 * apply PostgreSQL's formula to each sample's input sizes.  The selectivities
 * do not depend on the input sizes and are computed once.
 */
typedef struct JoinSizeSelectivities
{
	JoinType	jointype;
	Selectivity fkselec;		/* FK-matched join clauses */
	Selectivity jselec;			/* the join's other own clauses */
	Selectivity pselec;			/* pushed-down clauses (outer joins only) */
} JoinSizeSelectivities;

extern void calc_joinrel_selectivities(PlannerInfo *root,
									   RelOptInfo *joinrel,
									   RelOptInfo *outer_rel,
									   RelOptInfo *inner_rel,
									   SpecialJoinInfo *sjinfo,
									   List *restrictlist,
									   JoinSizeSelectivities *sel);
extern double calc_joinrel_rows(const JoinSizeSelectivities *sel,
								double outer_rows, double inner_rows,
								double error_factor);

#endif // COST_EXT_H
