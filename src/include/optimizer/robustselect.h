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
 *
 * The objectives score a path by its cost vectors (optimizer/robustcost.h),
 * the lower the better: a loss x(s) per scenario, summed up over the N
 * scenarios (the paper's Section 4.1; the numbering is v20's):
 *
 *	0	worst penalty			max_s Penalty(s)
 *	1	expected penalty		E[Penalty]
 *	2	worst total cost		max_s total(s)
 *	3	expected total cost 	E[Cost] (ranks as the paper's E[Gap])
 *	4	worst startup cost		max_s startup(s)
 *	5	expected startup cost	E[startup]
 *	6	penalty probability 	E[Penalty(s) > 0] = 1 - ProbNoPenalty
 *	8	point total cost		PostgreSQL's estimate
 *	10	worst penalty + lambda Stdev[Penalty]
 *	11	E[Penalty] + lambda Stdev[Penalty]
 *	12	Quant_alpha[Penalty]
 *	17	Quant_alpha[Cost]
 *	18	Quant_alpha[startup]
 *
 * with Penalty(s) = total(s) - C*(s) if total(s) > (1 + robust_penalty_tau)
 * C*(s), else 0, against the relation's envelope (optimizer/robustsearch.h):
 * that of its complete paths for the pathlist, that of its partial paths
 * for the partial_pathlist (a partial path's costs are per worker).  Stdev
 * is the population's over the N scenarios, lambda robust_penalty_lambda;
 * Quant_alpha the ceil(alpha N)-th smallest, alpha robust_quantile_alpha
 * (0.85: v20's "4th worst" at N = 20).  The maxima start at 0, as v20's.
 *
 * The diversification functions (Section 4.2) pick the retained paths and
 * are the retain strategy only (retain_strategy_id); from the candidates
 * the main objective did not keep they pick, one at a time:
 *
 *	7	Rand			uniformly, from a stream seeded by error_sample_seed
 *						and the relation
 *	13	Similarity		farthest-first k-center by the Jensen-Shannon
 *						distance of the total cost vectors, from the least
 *						E[Cost], among the paths not above theta = 2 max_s
 *						C*(s) in every scenario
 *	14	Mix				objectives 3, 1, 11, 12, 6 and functions 13, 16 in
 *						turn, the budget shared evenly
 *	15	E[Penalty]-Join the best of each last operator (join method, or
 *	16	E[Cost]-Join	scan), the operators in turn
 *
 * An objective as the retain strategy keeps its best of those candidates.
 * 9 (v20's "jointype score") is not implemented, in v20 either.
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
 * Above the join search (S9):
 *
 *	- the scan/join relation, once planner.c has applied the final target to
 *	  its paths and built the top Gather paths (apply_scanjoin_target_to_
 *	  paths), has all its paths scored by the main objective against the
 *	  cheapest of them in each scenario, without pruning
 *	  (robust_select_score_rel): the projections of the paths the selection
 *	  kept and the Gather paths on its kept partial paths, or every path of
 *	  the only relation of a query without joins;
 *	- the upper relations (grouping, window, distinct, ordered, final) keep
 *	  the paths no other dominates in every scenario (add_path in the upper
 *	  pass), costed per scenario by replay (optimizer/robustcost.h);
 *	- the plan of each query level is made from the path of its final
 *	  relation that is the best by the main objective of F(s), the cost of
 *	  fetching the fraction of its rows the caller asks for
 *	  (get_cheapest_fractional_path; 0, all of them, for a query's top
 *	  level, whose LIMIT is a Limit path costed for the rows it fetches),
 *	  with Penalty(s) against the cheapest F(s) of the final relation's
 *	  paths (robust_select_final; the startup objectives score startup(s),
 *	  8 the point estimate of F).  When none of them has vectors,
 *	  PostgreSQL's choice stands.
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
extern void robust_select_score_rel(PlannerInfo *root, RelOptInfo *rel);
extern Path *robust_select_final(RelOptInfo *rel, double tuple_fraction,
								 Path *pg);

#endif							/* ROBUSTSELECT_H */
