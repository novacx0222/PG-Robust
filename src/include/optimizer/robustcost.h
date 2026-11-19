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
} RobustCostStatus;

typedef struct RobustPathCost
{
	int			id;				/* unique within the planning of a query */
	RobustCostStatus status;
	bool		partial;		/* offered to add_partial_path */
	bool		semi_frozen;	/* semi-join factors kept at the point */
	RobustVec  *rows;			/* NULL: the scalar in every scenario */
	RobustVec  *startup;
	RobustVec  *total;
	int		   *nbatches;		/* HashPath: num_batches per scenario, or
								 * NULL */
	double		replayed[3];	/* MISMATCH: rows, startup, total the point
								 * replay gave */
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
									 const SemiAntiJoinFactors *semifactors);
extern void robust_cost_add_path(Path *path, bool partial);
extern void robust_cost_partial_rows(Path *path);
extern void robust_cost_append_rows(Path *path);
extern void robust_cost_memoize_reparameterized(MemoizePath *copy,
												List *param_exprs);
extern void robust_cost_bucket_filled(RestrictInfo *rinfo, bool right,
									  Path *path, double virtualbuckets);

#endif							/* ROBUSTCOST_H */
