/*-------------------------------------------------------------------------
 *
 * robustcost.h
 *	  Costs of paths in the error scenarios of RobDP ("replay").
 *
 * The cost of a path in scenario s is PostgreSQL's own cost function for
 * the path, run again with every input it reads set to its value in s:
 *
 *	- the row count of the path's relation, or of its ParamPathInfo, and of
 *	  every base relation (read by get_loop_count, estimate_num_groups,
 *	  estimate_hash_bucket_stats, eqjoinsel_semi), from their row vectors
 *	  (robustrows.h);
 *	- the rows, startup and total cost of its input paths, from their own
 *	  vectors, and the inputs' own scenario-dependent fields (the batches of
 *	  a hash join, the calls of a Memoize, i.e. its outer side's rows);
 *	- the selectivities a cost function estimates by itself, scaled by the
 *	  multipliers of the querylets whose clauses they cover: the index
 *	  selectivity of amcostestimate (btree and generic), the tuples passing
 *	  the merge or hash clauses (approx_tuple_count), the semi-join factors
 *	  (compute_semi_anti_join_factors).
 *
 * A querylet's multiplier applies to such a selectivity when one of the
 * selectivity's clauses belongs to the querylet: a restriction clause of
 * base relation r (r's querylet), a join clause between two base relations
 * (their pair querylet), a clause of an equivalence class with 3 or more
 * member relations (the path's class factor).  The querylets are those the
 * path's row count was computed with: the relation's or the ParamPathInfo's
 * for a scan, those of the pair joined for a join.
 *
 * Paths above the join search are replayed as their create_*_path
 * functions cost them (Agg, Group, Unique, WindowAgg, SetOp, Limit,
 * LockRows, ModifyTable, and the Gather paths of a partial aggregation,
 * given the rows of their input times its workers).  A grouping path's
 * number of groups is estimate_num_groups' in the scenario, from the rows
 * of its input and of the base relations (RobustGroups says how).  What a
 * constructor is given and the path does not keep -- an Agg's
 * AggClauseCosts, a WindowAgg's window functions, a Limit's estimates of
 * its offset and count -- is recorded by the constructor.  The cost of a
 * query level's initPlans, which SS_charge_for_initplans adds to its final
 * relation's paths once they are costed, is added to their vectors as well
 * (robust_cost_charge); so is the change of target apply_projection_to_path
 * makes in place (robust_cost_project).  GroupingSets and RecursiveUnion are
 * not replayed
 * (status unsupported); MinMaxAgg, costed from its subqueries' plans, is
 * constant.
 *
 * Decisions taken while costing that shape the plan are kept at the point
 * estimate's choice (a merge join's materialize_inner, a Unique's method);
 * those that do not are replayed (a hash join's batches).  A hash clause's
 * bucket estimate, which PostgreSQL caches in the RestrictInfo for the
 * number of buckets of the first hash join costed with it, is redone with
 * the scenario's rows and the number of buckets that first hash join has in
 * the scenario.
 *
 * The scalar fields (path->rows, startup_cost, total_cost) stay
 * PostgreSQL's.  Path.robust holds the vectors, NULL when the value is the
 * scalar in every scenario.  A replay first runs once with nothing changed
 * and must reproduce the scalar fields bit for bit; if it does not, the
 * path keeps constant vectors and the debug dump reports it.
 *
 * In an envelope round (optimizer/robustsearch.h) the rows of the
 * relations and the costs of the level-1 relations' paths are in the
 * round's scenario s already, and the round's paths get their costs in s in
 * their scalar fields, with no vectors.  With robust_projected_rounds on
 * (the default), PostgreSQL's cost functions compute these costs as the
 * paths are built.  What is not in s yet are the multipliers of the
 * estimates a join path's cost functions make themselves, and two hooks
 * apply them: robust_project_qual_selec (approx_tuple_count) and
 * robust_cost_join_context (the semi-join factors the join's paths are
 * given).  Index and bitmap heap paths, whose cost functions estimate an
 * index selectivity, are replayed as before (a round builds them only by
 * reparameterize_path, for the children of an Append); no other path of a
 * round is.  The costs are those of a replay in s, bit for bit (with
 * robust_debug_check_costs on, every path is replayed as well and any
 * difference is an error), but for one decision: a merge join's
 * materialize_inner is chosen in s, as PostgreSQL chooses it, where the
 * replay keeps the choice made without the multipliers.  The replay's
 * choice never costs less in s; the check counts the paths where the two
 * differ.  With robust_projected_rounds off, each path of a round is costed
 * without the multipliers, then replayed in s.
 *
 * Join paths (nested loop, merge and hash joins, with the rescan of a
 * nested loop's inner side, Memoize included) are not replayed scenario by
 * scenario but costed by vector versions of their cost functions
 * (robust_vector_cost, see vector_join in robustcost.c): what does not
 * depend on the scenario is computed once per path, and the rest is
 * PostgreSQL's arithmetic on the scenario's inputs, written so that it
 * gives the replay's results bit for bit.  Like a replay they start at the
 * point, and a path whose point is not its scalars is replayed.  With
 * robust_debug_check_costs on (the default in assert-enabled builds), every
 * path costed so is replayed too, and any difference is an error; so is an
 * estimate found in one of the caches of estimates in a scenario (hash
 * bucket statistics, numbers of groups) that is not what the estimator
 * gives anew.
 *
 * src/include/optimizer/robustcost.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ROBUSTCOST_H
#define ROBUSTCOST_H

#include "nodes/pathnodes.h"
#include "optimizer/robustvec.h"

/* How a path's vectors were obtained (RobustPathCost.status) */
typedef enum RobustCostStatus
{
	ROBUST_COST_CONST,			/* no input varies: the scalars */
	ROBUST_COST_REPLAY,			/* replayed */
	ROBUST_COST_MISMATCH,		/* the replay did not reproduce the scalars */
	ROBUST_COST_UNSUPPORTED,	/* a path type replay does not know */
	ROBUST_COST_NOCONTEXT,		/* a join path costed outside its join */
	ROBUST_COST_PROJECTED,		/* an envelope round's, costed in its
								 * scenario as it was built */
} RobustCostStatus;

/*
 * How an upper path's number of groups is obtained in a scenario
 * (RobustPathCost.groups): PostgreSQL computes it before creating the path
 * and does not record how, so the replay tries these in order and takes
 * the first that reproduces the point.  The input rows are those of the
 * path's input path, or of the relation the grouping reads (below the
 * upper relations: the scan/join relation, whose rows get_number_of_groups
 * reads also for a partial aggregation's finalize step).
 */
typedef enum RobustGroups
{
	ROBUST_GROUPS_NONE = 0,		/* not a grouping path */
	ROBUST_GROUPS_EST_SUB,		/* estimate_num_groups, input path's rows */
	ROBUST_GROUPS_EST_INPUT,	/* estimate_num_groups, input relation's rows */
	ROBUST_GROUPS_SUB_ROWS,		/* the input path's rows */
	ROBUST_GROUPS_INPUT_ROWS,	/* the input relation's rows */
	ROBUST_GROUPS_FROZEN,		/* the point's in every scenario */
} RobustGroups;

typedef struct RobustPathCost
{
	int			id;				/* unique within the planning of a query */
	RobustCostStatus status;
	bool		partial;		/* offered to add_partial_path */
	bool		semi_frozen;	/* semi-join factors kept at the point */
	RobustGroups groups;		/* upper grouping paths */
	RobustVec  *rows;			/* NULL: the scalar in every scenario */
	RobustVec  *startup;
	RobustVec  *total;
	int		   *nbatches;		/* HashPath: num_batches per scenario, or
								 * NULL */
	double		replayed[3];	/* MISMATCH: rows, startup, total the point
								 * replay gave */
	/* set by the selection of the main pass (optimizer/robustselect.h) */
	bool		scored;
	double		score;			/* main objective, against its relation's
								 * envelope */
	double		mean_startup;	/* expected startup and total cost */
	double		mean_total;
} RobustPathCost;

/*
 * The state the hooks inside cost functions read; robust_replay is NULL
 * except while a replay runs.
 */
typedef struct RobustReplayHooks
{
	double		index_mult;		/* scales amcostestimate's selectivity */
	List	   *quals;			/* the clauses approx_tuple_count is */
	double		qual_mult;		/* expected for, and their factor */
	double		semi_mult;		/* scales the semi-join selectivities */
} RobustReplayHooks;

extern PGDLLIMPORT RobustReplayHooks *robust_replay;

/*
 * The scenario of the projected envelope round running (see above), or -1:
 * the cost functions' hooks scale their own estimates as in that scenario.
 */
extern PGDLLIMPORT int robust_project_s;

extern Selectivity robust_replay_index_selec(Selectivity sel);
extern Selectivity robust_replay_qual_selec(List *quals, Selectivity sel);
extern void robust_replay_semi_selec(Selectivity *jselec, Selectivity *nselec);
extern void robust_replay_bucket_stats(PlannerInfo *root, RestrictInfo *rinfo,
									   bool right, double virtualbuckets,
									   Selectivity *mcvfreq,
									   Selectivity *bucketsize);
struct EstimationInfo;
extern double robust_replay_num_groups(PlannerInfo *root, List *exprs,
									   double input_rows,
									   struct EstimationInfo *estinfo);
extern Selectivity robust_project_qual_selec(PlannerInfo *root,
											 JoinPath *path, List *quals,
											 Selectivity sel);

/* Planner entry points */
extern void *robust_cost_begin_query(void);
extern void robust_cost_end_query(void *saved);
extern PlannerInfo *robust_cost_get_root(void);
extern void robust_cost_set_root(PlannerInfo *root);
extern void robust_cost_join_context(PlannerInfo *root, RelOptInfo *joinrel,
									 RelOptInfo *outerrel,
									 RelOptInfo *innerrel, JoinType jointype,
									 SpecialJoinInfo *sjinfo,
									 List *restrictlist, bool inner_unique,
									 SemiAntiJoinFactors *semifactors);
extern void robust_cost_add_path(Path *path, bool partial);
extern void robust_cost_partial_rows(Path *path);
extern void robust_cost_append_rows(Path *path);
extern void robust_cost_memoize_reparameterized(MemoizePath *copy,
												List *param_exprs);
extern void robust_cost_bucket_filled(RestrictInfo *rinfo, bool right,
									  Path *path, double virtualbuckets);
extern void robust_cost_agg_costs(AggPath *path,
								  const AggClauseCosts *aggcosts);
extern void robust_cost_window_funcs(WindowAggPath *path, List *windowFuncs);
extern void robust_cost_limit_ests(LimitPath *path, int64 offset_est,
								   int64 count_est);
extern void robust_cost_charge(PlannerInfo *root, RelOptInfo *final_rel,
							   Cost cost);
extern void robust_cost_ordered_limit(PlannerInfo *root, double limit_tuples);
extern void robust_cost_project(PlannerInfo *root, Path *path,
								PathTarget *target);

/* The join search (robustsearch.c, pathnode.c) */
extern void robust_cost_note_varies(void);
extern bool robust_cost_dominance(void);
extern bool robust_cost_precheck_off(void);
extern void *robust_cost_search_save(void);
extern void robust_cost_search_restore(void *save);
extern const int *robust_cost_round_counts(void);
extern int	robust_cost_round_flips(void);
extern void robust_cost_round_project(int s);
extern bool robust_cost_scores_active(void);
extern bool robust_cost_collect(void);

#endif							/* ROBUSTCOST_H */
