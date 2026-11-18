/*-------------------------------------------------------------------------
 *
 * cost_check.h
 *	  Debug-only cross check of the RobDP 1p and 2p cost functions.
 *
 * See src/backend/optimizer/path/cost_check.c for the invariant checked and
 * how mismatches are reported.
 *
 * src/include/optimizer/cost_check.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef COST_CHECK_H
#define COST_CHECK_H

#include "nodes/pathnodes.h"

/* GUC: robust_debug_check_costs */
extern PGDLLIMPORT bool robust_debug_check_costs;

/*
 * Per-query-level counters; PlannerInfo.robust_check points to one of these
 * while the check is enabled.  The contents are private to cost_check.c.
 */
typedef struct RobustCheckStats RobustCheckStats;

extern void robust_check_begin(PlannerInfo *root);
extern void robust_check_report(PlannerInfo *root);
extern void robust_check_note_fill(PlannerInfo *root, const Path *path);
extern void robust_check_envelope(PlannerInfo *root, const RelOptInfo *rel,
								  const double *envelope);

/*
 * One entry point per 2p cost function.  Each is called right after the 2p
 * function returned, with the same arguments, and only reports.
 */
extern void robust_check_seqscan(PlannerInfo *root, const Path *path,
								 const RelOptInfo *baserel,
								 const ParamPathInfo *param_info);
extern void robust_check_index(PlannerInfo *root, const IndexPath *path,
							   double loop_count, bool partial_path);
extern void robust_check_gather(PlannerInfo *root, const GatherPath *path,
								const RelOptInfo *rel,
								const ParamPathInfo *param_info,
								const double *rows);
extern void robust_check_gather_merge(PlannerInfo *root,
									  const GatherMergePath *path,
									  const RelOptInfo *rel,
									  const ParamPathInfo *param_info,
									  const double *rows);
extern void robust_check_nestloop(PlannerInfo *root, const NestPath *path,
								  const JoinCostWorkspace *workspace,
								  JoinPathExtraData *extra);
extern void robust_check_mergejoin(PlannerInfo *root, const MergePath *path,
								   JoinPathExtraData *extra);
extern void robust_check_hashjoin(PlannerInfo *root, const HashPath *path,
								  JoinPathExtraData *extra);

#endif							/* COST_CHECK_H */
