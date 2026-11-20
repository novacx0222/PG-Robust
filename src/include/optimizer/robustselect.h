/*-------------------------------------------------------------------------
 *
 * robustselect.h
 *	  Selection of the paths a relation of RobDP's main pass keeps.
 *
 * As in v20 (the implementation the paper's experiments ran), the main
 * pass collects every candidate path of a join relation (add_path keeps
 * them all, unless robust_dominance prunes dominated ones first) and, once
 * the relation is complete, keeps:
 *
 *	- the add_path_limit best by the main objective (main_objective_id),
 *	- of the others, the retain_path_limit best by the retain strategy
 *	  (retain_strategy_id),
 *	- and, if none of those is unparameterized, the best unparameterized
 *	  candidate, so that the relation keeps a complete plan,
 *
 * and, at the final relation, robust_final_path_limit by the main objective
 * (1, as in v20) and no retained path.
 * Ties go to the lower expected total cost, then as in compare_path_costs
 * (point total, then startup cost), then to the earlier candidate.
 * The objectives score a path by its cost vectors (optimizer/robustcost.h):
 *
 *	0	worst penalty		max_s Penalty(s)
 *	1	expected penalty	mean_s Penalty(s)
 *	2	worst total cost	max_s total(s)
 *	3	expected total cost mean_s total(s)
 *	4	worst startup cost	max_s startup(s)
 *	5	expected startup cost mean_s startup(s)
 *
 * with Penalty(s) = total(s) - C*(s) if total(s) > (1 + robust_penalty_tau)
 * C*(s), else 0, against the relation's envelope (optimizer/robustsearch.h):
 * that of its complete paths for the pathlist, that of its partial paths
 * for the partial_pathlist (a partial path's costs are per worker).  The
 * numbering is v20's; v20's other objectives are not implemented yet.
 *
 * The partial paths are selected first, the Gather paths built on top of
 * those kept, and then the complete paths, Gather paths included.  The
 * paths kept get their score (RobustPathCost.score), and compare_path_costs
 * compares two scored paths by it (then by the expected cost), so the
 * cheapest paths of the relation (set_cheapest, get_cheapest_path_for_
 * pathkeys) are the best by the objective.  The level-1 relations of the
 * search are scored but not pruned.
 *
 * With robust_join_pairing = retained, a join is built from every path its
 * inputs keep, where PostgreSQL uses only a few of them (the cheapest
 * total path, ...): the paper's P(U1) x P(U2) x J (see joinpath.c).
 *
 * src/include/optimizer/robustselect.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ROBUSTSELECT_H
#define ROBUSTSELECT_H

#include "nodes/pathnodes.h"

extern bool robust_objective_needs_envelope(int objective);
extern bool robust_select_retained(PlannerInfo *root);
extern bool robust_select_offer(List **pathlist, Path *path);
extern void robust_select_paths(PlannerInfo *root, RelOptInfo *rel,
								bool partial, int limit, int retain,
								const double *envelope);
extern int	robust_select_compare(Path *path1, Path *path2,
								  CostSelector criterion);

#endif							/* ROBUSTSELECT_H */
