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
 * With robust_select_dedup on, a candidate is not collected when the list
 * already has its twin: the same parameterization, pathkeys, parallel
 * safety and workers, and rows, startup and total cost in every scenario
 * (the join builders meet the same inputs more than once under retained
 * pairing).  The twins are looked up in an index of the list by the point
 * rows and costs (robust_select_offer), so collecting stays linear in the
 * number of candidates; with robust_debug_check_costs on, the list is
 * scanned as well, and a different answer is an error.
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
 *	6	penalty count			#{s : Penalty(s) > 0} = N (1 - ProbNoPenalty),
 *							0 to N (v20's coverage count reversed)
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
 * With robust_select_precheck on, the join builders ask
 * robust_precheck_join() before they build and cost a join path
 * candidate, in place of add_path_precheck(), which the main pass turns
 * off.  It drops a candidate the selection could never keep: from its
 * input paths' costs in each scenario a lower bound of its total (and
 * startup) cost in each scenario follows (nested loop: the outer path's
 * total plus the inner's startup; hash join: both totals; merge join: each
 * side's startup, its total when it is sorted), and from those, as every
 * objective but 10 and 11 is monotone in them, a lower bound L of its
 * score.  The candidate is dropped when, among the candidates offered so
 * far, at least K have a main score below L (K = add_path_limit, or
 * add_path_limit + retain_path_limit when the retain strategy is the main
 * objective; robust_final_path_limit if larger), at least
 * add_path_limit + retain_path_limit a retain score below its bound
 * (another objective as the retain strategy), and, for a complete
 * unparameterized candidate, one unparameterized candidate a main score
 * below L (one parallel-safe one too, for a parallel-safe candidate).  The
 * selection then keeps exactly what it would have kept.  Not with a
 * diversification function as the retain strategy, objectives 10 and 11,
 * child relations of partitionwise joins, or when an objective needs the
 * envelope and the relation's lacks a scenario (the selection would fill
 * it from the candidates).  The bounds are lowered by a relative 1e-9 for
 * the rounding of the cost functions' sums.
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
extern bool robust_select_offer(RelOptInfo *rel, bool partial, Path *path);

/* The join method of a candidate robust_precheck_join() bounds */
typedef enum RobustJoinBound
{
	ROBUST_BOUND_NESTLOOP,
	ROBUST_BOUND_HASHJOIN,
	ROBUST_BOUND_MERGEJOIN,
} RobustJoinBound;

extern bool robust_precheck_join(PlannerInfo *root, RelOptInfo *joinrel,
								 bool partial, RobustJoinBound method,
								 Path *outer_path, Path *inner_path,
								 bool outer_sorted, bool inner_sorted,
								 bool parameterized);
extern void robust_select_paths(PlannerInfo *root, RelOptInfo *rel,
								bool partial, int limit, int retain,
								const double *envelope);
extern int	robust_select_compare(Path *path1, Path *path2,
								  CostSelector criterion);
extern void robust_select_score_rel(PlannerInfo *root, RelOptInfo *rel);
extern Path *robust_select_final(RelOptInfo *rel, double tuple_fraction,
								 Path *pg);

#endif							/* ROBUSTSELECT_H */
