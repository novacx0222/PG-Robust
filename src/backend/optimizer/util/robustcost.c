/*-------------------------------------------------------------------------
 *
 * robustcost.c
 *	  Costs of paths in the error scenarios of RobDP, by replaying
 *	  PostgreSQL's cost functions (see robustcost.h for the model).
 *
 * A replay works on a bitwise copy of the path, so the cost functions can
 * write their results (and their own fields: an index path's selectivity, a
 * hash join's batches) without touching the path.  The inputs are set in
 * place for each scenario -- the rows fields of the relations and of the
 * ParamPathInfo, the rows and costs of the input paths -- and restored
 * afterwards.  Everything a cost function allocates goes to a scratch
 * context, reset after each scenario.
 *
 * src/backend/optimizer/util/robustcost.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "common/hashfn.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/robust.h"
#include "optimizer/robustcost.h"
#include "optimizer/robustrows.h"
#include "utils/hsearch.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"

RobustReplayHooks *robust_replay = NULL;

static RobustReplayHooks hooks;
static const Path *replay_path = NULL;	/* the path a replay is for */
static int	replay_s = -1;		/* and its scenario, -1 for the point */
static MemoryContext scratch_cxt = NULL;

/*
 * The join add_paths_to_joinrel() is building paths for: what the cost
 * functions of its join paths read besides the paths themselves.  The
 * semi-join factors of each scenario and the querylets of the pairs of
 * input paths are computed on first use.
 */
typedef struct JoinContext
{
	bool		valid;
	PlannerInfo *root;
	RelOptInfo *joinrel;
	RelOptInfo *outerrel;
	RelOptInfo *innerrel;
	JoinType	jointype;
	SpecialJoinInfo *sjinfo;
	List	   *restrictlist;
	bool		inner_unique;
	bool		has_semi;		/* semifactors were computed */
	SemiAntiJoinFactors semifactors;	/* PostgreSQL's */
	SemiAntiJoinFactors *sf;	/* per scenario, NULL until computed */
	bool		sf_frozen;		/* the replay did not reproduce semifactors */
	List	   *pair_qs;		/* PairQuerylets */
} JoinContext;

/* The querylets of a join of input paths parameterized by oreq and ireq */
typedef struct PairQuerylets
{
	Relids		oreq;
	Relids		ireq;
	Relids		req;
	List	   *qs;
} PairQuerylets;

/*
 * The state of the planning of one query (standard_planner).  A planning
 * started while another runs (a function evaluated during planning) gets
 * its own, and the outer one's is restored when it ends.
 */
typedef struct CostState
{
	PlannerInfo *root;			/* the query level building paths */
	int			next_path_id;
	JoinContext jcx;
	MemoryContext cxt;			/* the planning's memory context */
	RelOptInfo *prows_rel;		/* add_paths_to_append_rel's partial_rows */
	Path	   *prows_path;		/* came from this path of prows_rel */
	List	   *append_paths;	/* Appends given their rows, and */
	List	   *append_rows;	/* their RobustVecs (NULL: constant) */
	List	   *memo_copies;	/* reparameterized Memoizes, and */
	List	   *memo_exprs;		/* the param_exprs they were costed with */
	HTAB	   *bucket_fill;	/* BucketFills */
	HTAB	   *bucket_cache;	/* estimate_hash_bucket_stats results */
	HTAB	   *groups_cache;	/* estimate_num_groups results */
	HTAB	   *upper_info;		/* UpperInfos */
	List	   *ordered_limits; /* OrderedLimits */
	bool		varies;			/* some row count or cost differs from the
								 * point estimate in some scenario */
	int			round_counts[ROBUST_COST_NOCONTEXT + 1];	/* statuses of
															 * envelope paths */
} CostState;

static CostState *st = NULL;

/* A hash clause's bucket filler (see robust_cost_bucket_filled) */
typedef struct BucketFillKey
{
	const RestrictInfo *rinfo;
	int			right;
} BucketFillKey;

typedef struct BucketFill
{
	BucketFillKey key;
	const Path *filler;			/* until its replay is done */
	double		vb;				/* the filler's buckets at the point */
	double	   *vbs;			/* per scenario, or NULL: vb in every one */
} BucketFill;

static BucketFill *bucket_fill(const RestrictInfo *rinfo, bool right,
							   bool create);

#define jcx (st->jcx)

/* ------------------------------------------------------------------ hooks */

/*
 * robust_replay_index_selec
 *		amcostestimate's selectivity of the index clauses, in the scenario
 *		being replayed.
 */
Selectivity
robust_replay_index_selec(Selectivity sel)
{
	double		p = sel * robust_replay->index_mult;

	CLAMP_PROBABILITY(p);
	return p;
}

/*
 * robust_replay_qual_selec
 *		approx_tuple_count's selectivity of quals, in the scenario being
 *		replayed.  Only the merge or hash clauses of the path being replayed
 *		are expected.
 */
Selectivity
robust_replay_qual_selec(List *quals, Selectivity sel)
{
	double		p;

	if (quals != robust_replay->quals)
		elog(ERROR, "RobDP: approx_tuple_count of unexpected clauses in a replay");
	p = sel * robust_replay->qual_mult;
	CLAMP_PROBABILITY(p);
	return p;
}

/*
 * robust_replay_semi_selec
 *		compute_semi_anti_join_factors' selectivities, in the scenario being
 *		replayed.
 */
void
robust_replay_semi_selec(Selectivity *jselec, Selectivity *nselec)
{
	double		j = *jselec * robust_replay->semi_mult;
	double		n = *nselec * robust_replay->semi_mult;

	CLAMP_PROBABILITY(j);
	CLAMP_PROBABILITY(n);
	*jselec = j;
	*nselec = n;
}

/* ------------------------------------------------------- planner state */

/*
 * robust_cost_begin_query
 *		A query is being planned (standard_planner); returns the state of
 *		the planning it is nested in, for robust_cost_end_query.
 */
void *
robust_cost_begin_query(void)
{
	CostState  *saved = st;

	robust_replay = NULL;
	st = palloc0(sizeof(CostState));
	st->cxt = CurrentMemoryContext;
	return saved;
}

void
robust_cost_end_query(void *saved)
{
	st = (CostState *) saved;
}

/* The query level whose paths are being built (query_planner) */
PlannerInfo *
robust_cost_get_root(void)
{
	return st ? st->root : NULL;
}

void
robust_cost_set_root(PlannerInfo *root)
{
	if (st)
		st->root = root;
}

void
robust_cost_join_context(PlannerInfo *root, RelOptInfo *joinrel,
						 RelOptInfo *outerrel, RelOptInfo *innerrel,
						 JoinType jointype, SpecialJoinInfo *sjinfo,
						 List *restrictlist, bool inner_unique,
						 const SemiAntiJoinFactors *semifactors)
{
	if (st == NULL)
		return;
	memset(&jcx, 0, sizeof(jcx));
	jcx.valid = true;
	jcx.root = root;
	jcx.joinrel = joinrel;
	jcx.outerrel = outerrel;
	jcx.innerrel = innerrel;
	jcx.jointype = jointype;
	jcx.sjinfo = sjinfo;
	jcx.restrictlist = restrictlist;
	jcx.inner_unique = inner_unique;
	jcx.has_semi = jointype == JOIN_SEMI || jointype == JOIN_ANTI || inner_unique;
	if (jcx.has_semi)
		jcx.semifactors = *semifactors;
}

/*
 * robust_cost_note_varies
 *		A row count or cost differs from the point estimate in some scenario
 *		(until then, every vector is the point and the join search is
 *		PostgreSQL's).
 */
void
robust_cost_note_varies(void)
{
	if (st)
		st->varies = true;
}

static inline RobustPass
current_pass(void)
{
	return (st && st->root) ? (RobustPass) st->root->robust_pass :
		ROBUST_PASS_NONE;
}

/*
 * robust_cost_dominance
 *		Do add_path() and add_partial_path() compare paths that have vectors
 *		scenario by scenario?  In every phase of robust planning but the
 *		envelope pass, whose paths have their round's costs in their scalar
 *		fields.
 */
bool
robust_cost_dominance(void)
{
	RobustPass	pass = current_pass();

	return pass == ROBUST_PASS_BASE || pass == ROBUST_PASS_MAIN ||
		pass == ROBUST_PASS_UPPER;
}

/*
 * robust_cost_precheck_off
 *		add_path_precheck() and add_partial_path_precheck() reject a join
 *		path by lower bounds of its point costs, which says nothing about
 *		its costs in the scenarios.  In the main pass, when some scenario
 *		differs from the point, they accept every path.  (The envelope pass
 *		keeps them: there the costs compared are the round's, and the lower
 *		bounds, computed from the inputs' costs in the round, are lower
 *		bounds of the round's costs.)
 */
bool
robust_cost_precheck_off(void)
{
	return st != NULL && current_pass() == ROBUST_PASS_MAIN &&
		(st->varies || robust_cost_collect());
}

/*
 * robust_cost_collect
 *		Do add_path() and add_partial_path() collect every candidate path,
 *		for the selection at the end of each join level to choose from?  In
 *		the main pass of a search that selects (robust_select), unless
 *		robust_dominance prunes the candidates first.
 */
bool
robust_cost_collect(void)
{
	return robust_select && !robust_dominance &&
		current_pass() == ROBUST_PASS_MAIN;
}

/*
 * robust_cost_scores_active
 *		Does compare_path_costs() compare two scored paths by their scores
 *		(optimizer/robustselect.h)?  Everywhere but in an envelope round,
 *		where the scalar fields hold the round's costs.
 */
bool
robust_cost_scores_active(void)
{
	RobustPass	pass = current_pass();

	return pass != ROBUST_PASS_NONE && pass != ROBUST_PASS_ENVELOPE;
}

/*
 * The envelope pass: robust_cost_search_save() before the first round,
 * robust_cost_search_restore() after each.  Paths and relations of a round
 * are freed with it, so the state that may point to them goes back to what
 * it was before the round: the Appends given their rows, the translated
 * Memoizes, the bucket fillers (the round's RestrictInfo caches are reset
 * too, see robustsearch.c), the join context.  The cache of bucket
 * estimates, keyed by RestrictInfo, is dropped (a round's partitionwise
 * joins translate clauses in the round's memory).
 */
typedef struct CostSave
{
	int			nappend;
	int			nmemo;
	List	   *fills;			/* copies of the BucketFills */
} CostSave;

void *
robust_cost_search_save(void)
{
	CostSave   *cs = palloc0(sizeof(CostSave));

	if (st == NULL)
		return cs;
	cs->nappend = list_length(st->append_paths);
	cs->nmemo = list_length(st->memo_copies);
	if (st->bucket_fill != NULL)
	{
		HASH_SEQ_STATUS hs;
		BucketFill *f;

		hash_seq_init(&hs, st->bucket_fill);
		while ((f = (BucketFill *) hash_seq_search(&hs)) != NULL)
		{
			BucketFill *c = palloc(sizeof(BucketFill));

			memcpy(c, f, sizeof(BucketFill));
			cs->fills = lappend(cs->fills, c);
		}
	}
	memset(st->round_counts, 0, sizeof(st->round_counts));
	return cs;
}

void
robust_cost_search_restore(void *save)
{
	CostSave   *cs = (CostSave *) save;
	ListCell   *lc;

	if (st == NULL)
		return;
	st->append_paths = list_truncate(st->append_paths, cs->nappend);
	st->append_rows = list_truncate(st->append_rows, cs->nappend);
	st->memo_copies = list_truncate(st->memo_copies, cs->nmemo);
	st->memo_exprs = list_truncate(st->memo_exprs, cs->nmemo);
	st->prows_rel = NULL;
	st->prows_path = NULL;
	jcx.valid = false;
	if (st->bucket_fill != NULL)
	{
		hash_destroy(st->bucket_fill);
		st->bucket_fill = NULL;
	}
	foreach(lc, cs->fills)
	{
		BucketFill *c = (BucketFill *) lfirst(lc);
		BucketFill *f = bucket_fill(c->key.rinfo, c->key.right, true);

		memcpy(f, c, sizeof(BucketFill));
	}
	if (st->bucket_cache != NULL)
	{
		hash_destroy(st->bucket_cache);
		st->bucket_cache = NULL;
	}
}

/* How the envelope pass's paths were costed: counts by RobustCostStatus */
const int *
robust_cost_round_counts(void)
{
	static const int zero[ROBUST_COST_NOCONTEXT + 1];

	return st ? st->round_counts : zero;
}

/* ------------------------------------------------- caches of estimates */

/*
 * Hashing of the cache keys word by word: dynahash's default hashes every
 * byte of a key, which showed in profiles of large joins.
 */
static inline uint64
mix64(uint64 x)
{
	x ^= x >> 30;
	x *= UINT64CONST(0xbf58476d1ce4e5b9);
	x ^= x >> 27;
	x *= UINT64CONST(0x94d049bb133111eb);
	x ^= x >> 31;
	return x;
}

static inline uint64
dbits(double d)
{
	uint64		u;

	memcpy(&u, &d, sizeof(u));
	return u;
}

/*
 * estimate_hash_bucket_stats in a replay.
 *
 * PostgreSQL estimates a hash clause's bucket size once, for the number of
 * buckets of the first hash join costed with the clause as inner key (the
 * "filler"), and caches it in the RestrictInfo.  In a scenario that number
 * is the filler's in the scenario: the replay of the filler, which runs
 * before any other hash join can use the cache, records it per scenario.
 */

static BucketFill *
bucket_fill(const RestrictInfo *rinfo, bool right, bool create)
{
	BucketFillKey k;

	if (st->bucket_fill == NULL)
	{
		HASHCTL		ctl;

		if (!create)
			return NULL;
		ctl.keysize = sizeof(BucketFillKey);
		ctl.entrysize = sizeof(BucketFill);
		ctl.hcxt = st->cxt;
		st->bucket_fill = hash_create("RobDP bucket fills", 64, &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	memset(&k, 0, sizeof(k));
	k.rinfo = rinfo;
	k.right = right;
	return (BucketFill *) hash_search(st->bucket_fill, &k,
									  create ? HASH_ENTER : HASH_FIND, NULL);
}

/* final_cost_hashjoin filled rinfo's cache for path */
void
robust_cost_bucket_filled(RestrictInfo *rinfo, bool right, Path *path,
						  double virtualbuckets)
{
	BucketFill *f;

	if (st == NULL)
		return;
	f = bucket_fill(rinfo, right, true);
	f->filler = path;
	f->vb = virtualbuckets;
	f->vbs = NULL;
}

/*
 * reparameterize_path_by_child translated a Memoize's param_exprs to the
 * child's Vars after the nested loop using it was costed (try_nestloop_path)
 * with the parent's: the replay estimates the distinct calls with those.
 */
void
robust_cost_memoize_reparameterized(MemoizePath *copy, List *param_exprs)
{
	MemoryContext old;

	if (st == NULL)
		return;
	old = MemoryContextSwitchTo(st->cxt);
	st->memo_copies = lappend(st->memo_copies, copy);
	st->memo_exprs = lappend(st->memo_exprs, param_exprs);
	MemoryContextSwitchTo(old);
}

/*
 * add_paths_to_append_rel gives its later partial Appends the rows of the
 * first one (partial_rows), whatever their own subpaths sum to: their
 * scenario rows are that path's.
 */
void
robust_cost_partial_rows(Path *path)
{
	if (st == NULL)
		return;
	st->prows_rel = path->parent;
	st->prows_path = path;
}

/*
 * What the constructors of some upper paths are given and the path does not
 * keep: an Agg's AggClauseCosts, a WindowAgg's window functions, a Limit's
 * estimates of its offset and count.  Recorded by the constructor, keyed by
 * the path (a later path at the same address records its own).
 */
typedef struct UpperInfo
{
	Path	   *path;			/* hash key */
	bool		has_aggcosts;
	AggClauseCosts aggcosts;
	List	   *window_funcs;
	int64		offset_est;
	int64		count_est;
} UpperInfo;

static UpperInfo *
upper_info(Path *path, bool create)
{
	UpperInfo  *u;
	bool		found;

	if (st->upper_info == NULL)
	{
		HASHCTL		ctl;

		if (!create)
			return NULL;
		ctl.keysize = sizeof(Path *);
		ctl.entrysize = sizeof(UpperInfo);
		ctl.hcxt = st->cxt;
		st->upper_info = hash_create("RobDP upper paths", 64, &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	u = (UpperInfo *) hash_search(st->upper_info, &path,
								  create ? HASH_ENTER : HASH_FIND, &found);
	if (create)
	{
		memset(u, 0, sizeof(UpperInfo));
		u->path = path;
	}
	return u;
}

void
robust_cost_agg_costs(AggPath *path, const AggClauseCosts *aggcosts)
{
	UpperInfo  *u;

	if (st == NULL)
		return;
	u = upper_info(&path->path, true);
	u->has_aggcosts = aggcosts != NULL;
	if (aggcosts != NULL)
		u->aggcosts = *aggcosts;
}

void
robust_cost_window_funcs(WindowAggPath *path, List *windowFuncs)
{
	if (st == NULL)
		return;
	upper_info(&path->path, true)->window_funcs = windowFuncs;
}

void
robust_cost_limit_ests(LimitPath *path, int64 offset_est, int64 count_est)
{
	UpperInfo  *u;

	if (st == NULL)
		return;
	u = upper_info(&path->path, true);
	u->offset_est = offset_est;
	u->count_est = count_est;
}

/*
 * robust_cost_charge
 *		SS_charge_for_initplans() has added the cost of the query level's
 *		initPlans to the startup and total cost of every path of its final
 *		relation, after their vectors were computed: add it in every scenario
 *		too (the initPlans' costs are their plans', the same in all).  The
 *		vectors are replaced, not changed, as another path may share them.
 *		The scores the selection gave keep their order (the same cost is
 *		added to all the paths of the relation); the expected costs move.
 *		One "charge" dump event lists the paths.
 */
void
robust_cost_charge(PlannerInfo *root, RelOptInfo *final_rel, Cost cost)
{
	StringInfoData ids;
	ListCell   *lc;

	if (!enable_rows_dist)
		return;
	initStringInfo(&ids);
	appendStringInfoChar(&ids, '[');
	foreach(lc, final_rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		RobustPathCost *rc = path->robust;

		if (rc == NULL)
			continue;
		for (int k = 0; k < 2; k++)
		{
			RobustVec **v = k == 0 ? &rc->startup : &rc->total;

			if (*v != NULL)
			{
				RobustVec  *c = robust_vec_copy(*v);

				for (int s = 0; s < c->n; s++)
					c->v[s] += cost;
				*v = c;
			}
		}
		if (rc->scored)
		{
			rc->mean_startup += cost;
			rc->mean_total += cost;
		}
		if (ids.len > 1)
			appendStringInfoChar(&ids, ',');
		appendStringInfo(&ids, "%d", rc->id);
	}
	appendStringInfoChar(&ids, ']');
	if (robust_dumping(root))
	{
		RobustDump	d;

		robust_dump_begin(&d, "charge");
		robust_dump_double(&d, "cost", cost);
		robust_dump_raw(&d, "ids", ids.data);
		robust_dump_end(&d);
	}
	pfree(ids.data);
}

static RobustPathCost *compute(PlannerInfo *root, Path *path, bool partial);

/*
 * The bound grouping_planner gives create_ordered_paths for its sorts: the
 * LIMIT's, also where root->limit_tuples is -1 (grouping, aggregates,
 * window functions below the ORDER BY).
 */
typedef struct OrderedLimit
{
	PlannerInfo *root;
	double		limit_tuples;
} OrderedLimit;

void
robust_cost_ordered_limit(PlannerInfo *root, double limit_tuples)
{
	OrderedLimit *o;

	if (st == NULL)
		return;
	o = MemoryContextAlloc(st->cxt, sizeof(OrderedLimit));
	o->root = root;
	o->limit_tuples = limit_tuples;
	st->ordered_limits = lcons(o, st->ordered_limits);
}

static bool
ordered_limit(PlannerInfo *root, double *limit_tuples)
{
	ListCell   *lc;

	foreach(lc, st->ordered_limits)
	{
		OrderedLimit *o = (OrderedLimit *) lfirst(lc);

		if (o->root == root)
		{
			*limit_tuples = o->limit_tuples;
			return true;
		}
	}
	return false;
}

void
robust_cost_append_rows(Path *path)
{
	RobustVec  *rows = NULL;
	MemoryContext old;

	if (st == NULL || st->root == NULL)
		return;
	if (st->prows_rel == path->parent && st->prows_path != NULL &&
		st->prows_path->rows == path->rows)
		rows = compute(st->root, st->prows_path, true)->rows;
	else
		rows = NULL;			/* not known: the given rows in every one */
	old = MemoryContextSwitchTo(st->cxt);
	st->append_paths = lappend(st->append_paths, path);
	st->append_rows = lappend(st->append_rows, rows);
	MemoryContextSwitchTo(old);
}

/* Was the Append given its rows, and which? */
static bool
append_given_rows(Path *path, RobustVec **rows)
{
	ListCell   *a,
			   *b;

	forboth(a, st->append_paths, b, st->append_rows)
	{
		if (lfirst(a) == path)
		{
			*rows = (RobustVec *) lfirst(b);
			return true;
		}
	}
	return false;
}

static List *
memo_costed_exprs(MemoizePath *mp)
{
	ListCell   *a,
			   *b;

	forboth(a, st->memo_copies, b, st->memo_exprs)
	{
		if (lfirst(a) == mp)
			return (List *) lfirst(b);
	}
	return mp->param_exprs;
}

/* the replay of a hash join is done: it can no longer be a filler */
static void
bucket_fills_done(HashPath *path)
{
	ListCell   *lc;

	if (st->bucket_fill == NULL)
		return;
	foreach(lc, path->path_hashclauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		for (int right = 0; right < 2; right++)
		{
			BucketFill *f = bucket_fill(rinfo, right, false);

			if (f != NULL && f->filler == &path->jpath.path)
				f->filler = NULL;
		}
	}
}

/*
 * The result depends on the scenario only through the buckets and the rows
 * of the base relation of the hash key, so it is cached under (clause,
 * side, buckets, those rows).
 */
typedef struct BucketKey
{
	const RestrictInfo *rinfo;
	double		vb;
	double		rows;
	int			right;
} BucketKey;

typedef struct BucketEntry
{
	BucketKey	key;
	Selectivity mcvfreq;
	Selectivity bucketsize;
} BucketEntry;

static uint32
bucket_hash(const void *key, Size keysize)
{
	const BucketKey *k = (const BucketKey *) key;
	uint64		h = mix64((uint64) (uintptr_t) k->rinfo ^ (uint64) k->right);

	h = hash_combine64(h, mix64(dbits(k->vb)));
	h = hash_combine64(h, mix64(dbits(k->rows)));
	return (uint32) (h ^ (h >> 32));
}

void
robust_replay_bucket_stats(PlannerInfo *root, RestrictInfo *rinfo, bool right,
						   double virtualbuckets, Selectivity *mcvfreq,
						   Selectivity *bucketsize)
{
	Node	   *key = right ? get_rightop(rinfo->clause) : get_leftop(rinfo->clause);
	Relids		relids = right ? rinfo->right_relids : rinfo->left_relids;
	double		vb = virtualbuckets;
	BucketFill *f = st ? bucket_fill(rinfo, right, false) : NULL;
	int			relid;
	BucketKey	k;
	BucketEntry *e;
	bool		found;

	if (f != NULL)
	{
		if (f->filler == replay_path)
		{
			/* the filler: its own buckets, recorded for the others */
			if (replay_s >= 0)
			{
				if (f->vbs == NULL)
				{
					f->vbs = MemoryContextAlloc(st->cxt,
												error_sample_count * sizeof(double));
					for (int i = 0; i < error_sample_count; i++)
						f->vbs[i] = f->vb;
				}
				f->vbs[replay_s] = virtualbuckets;
			}
		}
		else if (f->vbs != NULL && replay_s >= 0)
			vb = f->vbs[replay_s];
		else
			vb = f->vb;
	}
	if (st == NULL || !bms_get_singleton_member(relids, &relid) ||
		relid >= root->simple_rel_array_size ||
		root->simple_rel_array[relid] == NULL)
	{
		estimate_hash_bucket_stats(root, key, vb, mcvfreq, bucketsize);
		return;
	}
	if (st->bucket_cache == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(BucketKey);
		ctl.entrysize = sizeof(BucketEntry);
		ctl.hash = bucket_hash;
		ctl.hcxt = st->cxt;
		st->bucket_cache = hash_create("RobDP bucket stats", 256, &ctl,
									   HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	}
	memset(&k, 0, sizeof(k));
	k.rinfo = rinfo;
	k.vb = vb;
	k.rows = root->simple_rel_array[relid]->rows;
	k.right = right;
	e = (BucketEntry *) hash_search(st->bucket_cache, &k, HASH_ENTER, &found);
	if (!found)
		estimate_hash_bucket_stats(root, key, vb, &e->mcvfreq, &e->bucketsize);
	*mcvfreq = e->mcvfreq;
	*bucketsize = e->bucketsize;
}

/*
 * estimate_num_groups of a Memoize's parameters in a replay.  When they
 * are plain Vars, the estimate depends on the scenario only through the
 * input rows and the rows of the Vars' relations, so it is cached under
 * those.
 */
#define GROUPS_MAXVARS 8

typedef struct GroupsKey
{
	int			nvars;
	int			varno[GROUPS_MAXVARS];
	int			attno[GROUPS_MAXVARS];
	double		rows[GROUPS_MAXVARS];	/* of varno[i]'s relation */
	double		input_rows;
} GroupsKey;

typedef struct GroupsEntry
{
	GroupsKey	key;
	double		ngroups;
	uint32		flags;
} GroupsEntry;

/* Unused entries are zero, so only the first nvars need hashing */
static uint32
groups_hash(const void *key, Size keysize)
{
	const GroupsKey *k = (const GroupsKey *) key;
	uint64		h = mix64(dbits(k->input_rows) ^ (uint64) k->nvars);

	for (int i = 0; i < k->nvars; i++)
	{
		h = hash_combine64(h, mix64(((uint64) (uint32) k->varno[i] << 32) |
									(uint32) k->attno[i]));
		h = hash_combine64(h, mix64(dbits(k->rows[i])));
	}
	return (uint32) (h ^ (h >> 32));
}

double
robust_replay_num_groups(PlannerInfo *root, List *exprs, double input_rows,
						 EstimationInfo *estinfo)
{
	GroupsKey	k;
	ListCell   *lc;
	GroupsEntry *e;
	bool		found;

	if (st == NULL || list_length(exprs) > GROUPS_MAXVARS)
		return estimate_num_groups(root, exprs, input_rows, NULL, estinfo);
	memset(&k, 0, sizeof(k));
	foreach(lc, exprs)
	{
		Node	   *n = (Node *) lfirst(lc);
		Var		   *v;

		while (IsA(n, RelabelType))
			n = (Node *) ((RelabelType *) n)->arg;
		if (!IsA(n, Var))
			return estimate_num_groups(root, exprs, input_rows, NULL, estinfo);
		v = (Var *) n;
		if (v->varlevelsup != 0 || v->varnullingrels != NULL ||
			v->varno >= root->simple_rel_array_size ||
			root->simple_rel_array[v->varno] == NULL)
			return estimate_num_groups(root, exprs, input_rows, NULL, estinfo);
		k.varno[k.nvars] = v->varno;
		k.attno[k.nvars] = v->varattno;
		k.rows[k.nvars] = root->simple_rel_array[v->varno]->rows;
		k.nvars++;
	}
	k.input_rows = input_rows;

	if (st->groups_cache == NULL)
	{
		HASHCTL		ctl;

		ctl.keysize = sizeof(GroupsKey);
		ctl.entrysize = sizeof(GroupsEntry);
		ctl.hash = groups_hash;
		ctl.hcxt = st->cxt;
		st->groups_cache = hash_create("RobDP num groups", 256, &ctl,
									   HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	}
	e = (GroupsEntry *) hash_search(st->groups_cache, &k, HASH_ENTER, &found);
	if (!found)
	{
		EstimationInfo info;

		memset(&info, 0, sizeof(info));
		e->ngroups = estimate_num_groups(root, exprs, input_rows, NULL, &info);
		e->flags = info.flags;
	}
	if (estinfo)
		estinfo->flags = e->flags;
	return e->ngroups;
}

/* ------------------------------------------------------- projection */

/* A double the replay sets to vec[s], and its saved value */
typedef struct Slot
{
	double	   *addr;
	double		saved;
	const RobustVec *vec;
} Slot;

/* An int set to vals[s] */
typedef struct ISlot
{
	int		   *addr;
	int			saved;
	const int  *vals;
} ISlot;

typedef struct Slots
{
	int			n;
	int			cap;
	Slot	   *a;
	int			ni;
	int			capi;
	ISlot	   *ai;
} Slots;

static void
slot_add(Slots *sl, double *addr, const RobustVec *vec)
{
	if (vec == NULL)
		return;
	if (sl->n == sl->cap)
	{
		sl->cap = sl->cap ? sl->cap * 2 : 16;
		sl->a = sl->a ? repalloc(sl->a, sizeof(Slot) * sl->cap) :
			palloc(sizeof(Slot) * sl->cap);
	}
	sl->a[sl->n].addr = addr;
	sl->a[sl->n].saved = *addr;
	sl->a[sl->n].vec = vec;
	sl->n++;
}

static void
islot_add(Slots *sl, int *addr, const int *vals)
{
	if (vals == NULL)
		return;
	if (sl->ni == sl->capi)
	{
		sl->capi = sl->capi ? sl->capi * 2 : 4;
		sl->ai = sl->ai ? repalloc(sl->ai, sizeof(ISlot) * sl->capi) :
			palloc(sizeof(ISlot) * sl->capi);
	}
	sl->ai[sl->ni].addr = addr;
	sl->ai[sl->ni].saved = *addr;
	sl->ai[sl->ni].vals = vals;
	sl->ni++;
}

static void
slots_project(Slots *sl, int s)
{
	for (int i = 0; i < sl->n; i++)
		*sl->a[i].addr = robust_vec_at(sl->a[i].vec, s);
	for (int i = 0; i < sl->ni; i++)
		*sl->ai[i].addr = sl->ai[i].vals[s];
}

/* Restore in reverse order, so a field added twice gets its first value */
static void
slots_restore(Slots *sl)
{
	for (int i = sl->n - 1; i >= 0; i--)
		*sl->a[i].addr = sl->a[i].saved;
	for (int i = sl->ni - 1; i >= 0; i--)
		*sl->ai[i].addr = sl->ai[i].saved;
}

/* Every base (or other member) relation with a row vector */
static void
add_base_rels(Slots *sl, PlannerInfo *root)
{
	for (int i = 1; i < root->simple_rel_array_size; i++)
	{
		RelOptInfo *rel = root->simple_rel_array[i];

		if (rel != NULL && rel->robust_rows != NULL)
			slot_add(sl, &rel->rows, rel->robust_rows);
	}
}

/* A relation's rows, unless it is a base relation (add_base_rels) */
static void
add_rel(Slots *sl, RelOptInfo *rel)
{
	if (rel->robust_rows != NULL &&
		rel->reloptkind != RELOPT_BASEREL &&
		rel->reloptkind != RELOPT_OTHER_MEMBER_REL)
		slot_add(sl, &rel->rows, rel->robust_rows);
}

/* Is any base relation of relids sized differently per scenario? */
static bool
base_rels_vary(PlannerInfo *root, Relids relids)
{
	int			i = -1;

	while ((i = bms_next_member(relids, i)) >= 0)
	{
		RelOptInfo *rel;

		if (i >= root->simple_rel_array_size)
			continue;
		rel = root->simple_rel_array[i];
		if (rel != NULL && rel->robust_rows != NULL)
			return true;
	}
	return false;
}

/* ------------------------------------------------- querylet multipliers */

/*
 * The querylet among qs a clause belongs to: the restriction querylet of
 * its base relation, the pair querylet of its two base relations, or the
 * class factor ("ec") for a clause of a multi-member equivalence class.
 */
static RobustQuerylet *
clause_querylet(PlannerInfo *root, RestrictInfo *rinfo, List *qs)
{
	Relids		rels = bms_difference(rinfo->clause_relids, root->outer_join_rels);
	int			n = bms_num_members(rels);
	Index		ia = 0;
	Index		ib = 0;
	bool		ec = false;
	ListCell   *lc;

	if (n == 1)
		ia = robust_instance_relid(root, bms_singleton_member(rels));
	else if (n == 2)
	{
		int			r1 = bms_next_member(rels, -1);
		int			r2 = bms_next_member(rels, r1);

		ia = robust_instance_relid(root, r1);
		ib = robust_instance_relid(root, r2);
		if (ia > ib)
		{
			Index		t = ia;

			ia = ib;
			ib = t;
		}
		ec = rinfo->parent_ec != NULL && robust_ec_is_star(root, rinfo->parent_ec);
	}
	bms_free(rels);
	if (ia == 0 || ia == ib)
		return NULL;

	foreach(lc, qs)
	{
		RobustQuerylet *q = (RobustQuerylet *) lfirst(lc);

		if (ec)
		{
			if (q->x == NULL && strcmp(q->key, "ec") == 0)
				return q;
		}
		else if (q->x != NULL && q->ia == ia && q->ib == ib)
			return q;
	}
	return NULL;
}

/*
 * The product of the multipliers of the querylets in qs that the clauses
 * (RestrictInfos) touch, each counted once, in the order of qs; NULL if
 * none is touched.
 */
static RobustVec *
touched_mult(PlannerInfo *root, List *clauses, List *qs)
{
	int			nq = list_length(qs);
	bool	   *touched;
	bool		any = false;
	RobustVec  *m;
	ListCell   *lc;

	if (nq == 0)
		return NULL;
	touched = palloc0(sizeof(bool) * nq);
	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		RobustQuerylet *q;

		if (!IsA(rinfo, RestrictInfo))
			continue;
		q = clause_querylet(root, rinfo, qs);
		if (q != NULL)
		{
			ListCell   *lq;

			foreach(lq, qs)
				if (lfirst(lq) == q)
					touched[foreach_current_index(lq)] = true;
			any = true;
		}
	}
	if (!any)
	{
		pfree(touched);
		return NULL;
	}
	m = robust_vec_make(error_sample_count);
	for (int s = 0; s < m->n; s++)
	{
		double		f = 1.0;

		foreach(lc, qs)
		{
			if (touched[foreach_current_index(lc)])
				f = f * robust_vec_at(((RobustQuerylet *) lfirst(lc))->mult, s);
		}
		m->v[s] = f;
	}
	pfree(touched);
	return m;
}

/* The querylets a scan's row count was computed with */
static List *
scan_querylets(Path *path)
{
	if (path->param_info != NULL)
		return path->param_info->ppi_robust_querylets;
	return path->parent->robust_querylets;
}

/* The index clauses of an index path, as RestrictInfos */
static List *
index_rinfos(IndexPath *ipath)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, ipath->indexclauses)
		result = lappend(result, ((IndexClause *) lfirst(lc))->rinfo);
	return result;
}

/*
 * The querylets of a join path's pair, as the row count of a join of its
 * inputs would be computed: the clauses' querylets and the class factor
 * M(J + req) M(oreq) M(ireq) / (M(req) M(O + oreq) M(I + ireq)).
 */
static List *
join_path_querylets(PlannerInfo *root, JoinPath *jp, SpecialJoinInfo *sjinfo)
{
	Relids		req = PATH_REQ_OUTER(&jp->path);
	Relids		oreq = PATH_REQ_OUTER(jp->outerjoinpath);
	Relids		ireq = PATH_REQ_OUTER(jp->innerjoinpath);
	PairQuerylets *pq;
	ListCell   *lc;

	/* The same for every path of the context with these parameterizations */
	if (jp->joinrestrictinfo == jcx.restrictlist)
	{
		foreach(lc, jcx.pair_qs)
		{
			pq = (PairQuerylets *) lfirst(lc);
			if (bms_equal(pq->oreq, oreq) && bms_equal(pq->ireq, ireq) &&
				bms_equal(pq->req, req))
				return pq->qs;
		}
	}

	{
		Relids		num[3] = {bms_union(jp->path.parent->relids, req), oreq, ireq};
		Relids		den[3] = {req,
			bms_union(jp->outerjoinpath->parent->relids, oreq),
		bms_union(jp->innerjoinpath->parent->relids, ireq)};
		List	   *qs = robust_join_querylets_ec(root, jp->path.parent, sjinfo,
												  jp->joinrestrictinfo,
												  num, 3, den, 3);

		if (jp->joinrestrictinfo == jcx.restrictlist)
		{
			pq = palloc(sizeof(PairQuerylets));
			pq->oreq = oreq;
			pq->ireq = ireq;
			pq->req = req;
			pq->qs = qs;
			jcx.pair_qs = lappend(jcx.pair_qs, pq);
		}
		return qs;
	}
}

/* -------------------------------------------------- semi-join factors */

/*
 * The semi-join factors of the join context in every scenario: PostgreSQL's
 * computation replayed with the rows of the scenario, on fresh copies of
 * the clauses (so the cached selectivities are neither used nor changed),
 * and its selectivities scaled by the non-pushed querylets of the pair.  If
 * the replay with nothing changed does not give PostgreSQL's factors (whose
 * cached selectivities may come from another pair), they are kept at
 * PostgreSQL's in every scenario.
 */
static void
compute_semifactors(void)
{
	PlannerInfo *root = jcx.root;
	int			N = error_sample_count;
	Relids		num = jcx.joinrel->relids;
	Relids		den[2] = {jcx.outerrel->relids, jcx.innerrel->relids};
	List	   *qs;
	RobustVec  *m;
	Slots		sl = {0};
	SemiAntiJoinFactors pt;
	MemoryContext oldcxt;
	ListCell   *lc;

	jcx.sf = palloc(sizeof(SemiAntiJoinFactors) * N);
	for (int s = 0; s < N; s++)
		jcx.sf[s] = jcx.semifactors;

	qs = robust_join_querylets_ec(root, jcx.joinrel, jcx.sjinfo,
								  jcx.restrictlist, &num, 1, den, 2);
	m = robust_vec_make(N);
	for (int s = 0; s < N; s++)
	{
		double		f = 1.0;

		foreach(lc, qs)
		{
			RobustQuerylet *q = (RobustQuerylet *) lfirst(lc);

			if (!q->pushed)
				f = f * q->mult->v[s];
		}
		m->v[s] = f;
	}
	add_base_rels(&sl, root);
	add_rel(&sl, jcx.outerrel);
	add_rel(&sl, jcx.innerrel);
	if (sl.n == 0 && qs == NIL)
		return;

	hooks.index_mult = 1.0;
	hooks.quals = NIL;
	hooks.qual_mult = 1.0;
	for (int s = -1; s < N; s++)
	{
		List	   *copies = NIL;
		SemiAntiJoinFactors *out = s < 0 ? &pt : &jcx.sf[s];

		oldcxt = MemoryContextSwitchTo(scratch_cxt);
		foreach(lc, jcx.restrictlist)
		{
			RestrictInfo *c = makeNode(RestrictInfo);

			memcpy(c, lfirst_node(RestrictInfo, lc), sizeof(RestrictInfo));
			c->norm_selec = -1;
			c->outer_selec = -1;
			copies = lappend(copies, c);
		}
		if (s >= 0)
			slots_project(&sl, s);
		hooks.semi_mult = s < 0 ? 1.0 : m->v[s];
		robust_replay = &hooks;
		compute_semi_anti_join_factors(root, jcx.joinrel, jcx.outerrel,
									   jcx.innerrel, jcx.jointype, jcx.sjinfo,
									   copies, out);
		robust_replay = NULL;
		if (s >= 0)
			slots_restore(&sl);
		MemoryContextSwitchTo(oldcxt);
		MemoryContextReset(scratch_cxt);

		if (s < 0 &&
			(pt.outer_match_frac != jcx.semifactors.outer_match_frac ||
			 pt.match_count != jcx.semifactors.match_count))
		{
			jcx.sf_frozen = true;
			for (int t = 0; t < N; t++)
				jcx.sf[t] = jcx.semifactors;
			break;
		}
	}
}

/* ------------------------------------------------------------ replay */

typedef struct TreeMult
{
	IndexPath  *ipath;
	RobustVec  *mult;			/* NULL: 1 */
} TreeMult;

typedef struct Replay
{
	PlannerInfo *root;
	Path	   *path;
	Size		size;			/* of the path's node type */
	Path	   *buf;			/* the copy the cost functions work on */
	Slots		slots;
	RobustVec  *index_mult;		/* IndexPath */
	List	   *tree;			/* BitmapHeapPath: TreeMults */
	List	   *quals;			/* MergePath/HashPath: approx_tuple_count's */
	RobustVec  *qual_mult;
	bool		join;
	double		limit_tuples;	/* SortPath, IncrementalSortPath */
	bool		trivial;		/* SubqueryScanPath */
	bool		loop_one;		/* BitmapHeapPath: built with loop_count 1
								 * for a lateral rel's own parameterization */
	MemoizePath *memo;			/* a Memoize inner whose est_entries */
	uint32		memo_entries;	/* cost_memoize_rescan overwrites */
	List	   *memo_exprs;		/* and whose param_exprs it gets */
	UpperInfo  *upper;			/* Agg, WindowAgg, Limit: what the
								 * constructor was given */
	RobustGroups groups;		/* grouping paths: how the groups are got */
	List	   *group_exprs;	/* the grouping expressions, or NIL */
	bool		have_exprs;		/* they were found in the input's target */
	RelOptInfo *input_rel;		/* the relation the grouping reads, or NULL */
	double		frozen_groups;	/* the point's groups, if known */
	bool		have_frozen;
	bool		gather_rows;	/* Gather, GatherMerge: given the rows of
								 * their input times its workers */
	double		got[3];			/* rows, startup, total of the last point
								 * replay */
} Replay;

/* The size of a path node of a type the replay knows; 0 for others */
static Size
path_size(Path *path)
{
	switch (nodeTag(path))
	{
		case T_Path:
			switch (path->pathtype)
			{
				case T_SeqScan:
				case T_SampleScan:
				case T_FunctionScan:
				case T_TableFuncScan:
				case T_ValuesScan:
				case T_CteScan:
				case T_WorkTableScan:
				case T_NamedTuplestoreScan:
				case T_Result:
					return sizeof(Path);
				default:
					return 0;
			}
		case T_IndexPath:
			return sizeof(IndexPath);
		case T_BitmapHeapPath:
			return sizeof(BitmapHeapPath);
		case T_TidPath:
			return sizeof(TidPath);
		case T_TidRangePath:
			return sizeof(TidRangePath);
		case T_SubqueryScanPath:
			return sizeof(SubqueryScanPath);
		case T_AppendPath:
			return sizeof(AppendPath);
		case T_MergeAppendPath:
			return sizeof(MergeAppendPath);
		case T_MaterialPath:
			return sizeof(MaterialPath);
		case T_MemoizePath:
			return sizeof(MemoizePath);
		case T_UniquePath:
			return sizeof(UniquePath);
		case T_GatherPath:
			return sizeof(GatherPath);
		case T_GatherMergePath:
			return sizeof(GatherMergePath);
		case T_SortPath:
			return sizeof(SortPath);
		case T_IncrementalSortPath:
			return sizeof(IncrementalSortPath);
		case T_ProjectionPath:
			return sizeof(ProjectionPath);
		case T_ProjectSetPath:
			return sizeof(ProjectSetPath);
		case T_NestPath:
			return sizeof(NestPath);
		case T_MergePath:
			return sizeof(MergePath);
		case T_HashPath:
			return sizeof(HashPath);
		case T_AggPath:
			return sizeof(AggPath);
		case T_GroupPath:
			return sizeof(GroupPath);
		case T_UpperUniquePath:
			return sizeof(UpperUniquePath);
		case T_WindowAggPath:
			return sizeof(WindowAggPath);
		case T_SetOpPath:
			return sizeof(SetOpPath);
		case T_LockRowsPath:
			return sizeof(LockRowsPath);
		case T_ModifyTablePath:
			return sizeof(ModifyTablePath);
		case T_LimitPath:
			return sizeof(LimitPath);
		default:
			return 0;
	}
}

/* The input paths of a path (of this query level) */
static List *
path_children(Path *path)
{
	switch (nodeTag(path))
	{
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			return list_make2(((JoinPath *) path)->outerjoinpath,
							  ((JoinPath *) path)->innerjoinpath);
		case T_AppendPath:
			return list_copy(((AppendPath *) path)->subpaths);
		case T_MergeAppendPath:
			return list_copy(((MergeAppendPath *) path)->subpaths);
		case T_MaterialPath:
			return list_make1(((MaterialPath *) path)->subpath);
		case T_MemoizePath:
			return list_make1(((MemoizePath *) path)->subpath);
		case T_UniquePath:
			return list_make1(((UniquePath *) path)->subpath);
		case T_GatherPath:
			return list_make1(((GatherPath *) path)->subpath);
		case T_GatherMergePath:
			return list_make1(((GatherMergePath *) path)->subpath);
		case T_SortPath:
			return list_make1(((SortPath *) path)->subpath);
		case T_IncrementalSortPath:
			return list_make1(((IncrementalSortPath *) path)->spath.subpath);
		case T_ProjectionPath:
			return list_make1(((ProjectionPath *) path)->subpath);
		case T_ProjectSetPath:
			return list_make1(((ProjectSetPath *) path)->subpath);
		case T_AggPath:
			return list_make1(((AggPath *) path)->subpath);
		case T_GroupPath:
			return list_make1(((GroupPath *) path)->subpath);
		case T_UpperUniquePath:
			return list_make1(((UpperUniquePath *) path)->subpath);
		case T_WindowAggPath:
			return list_make1(((WindowAggPath *) path)->subpath);
		case T_SetOpPath:
			return list_make1(((SetOpPath *) path)->subpath);
		case T_LockRowsPath:
			return list_make1(((LockRowsPath *) path)->subpath);
		case T_ModifyTablePath:
			return list_make1(((ModifyTablePath *) path)->subpath);
		case T_LimitPath:
			return list_make1(((LimitPath *) path)->subpath);
		/* not replayed (status unsupported), but their inputs are */
		case T_GroupingSetsPath:
			return list_make1(((GroupingSetsPath *) path)->subpath);
		case T_RecursiveUnionPath:
			return list_make2(((RecursiveUnionPath *) path)->leftpath,
							  ((RecursiveUnionPath *) path)->rightpath);
		default:
			return NIL;
	}
}


/* Does a computed path vary per scenario? */
static bool
cost_varies(const RobustPathCost *rc)
{
	return rc != NULL &&
		(rc->rows != NULL || rc->startup != NULL || rc->total != NULL ||
		 rc->nbatches != NULL);
}

/* Set a path's rows and costs to the scenario's */
static void
add_path_slots(Slots *sl, Path *p)
{
	RobustPathCost *rc = p->robust;

	if (rc == NULL)
		return;
	slot_add(sl, &p->rows, rc->rows);
	slot_add(sl, &p->startup_cost, rc->startup);
	slot_add(sl, &p->total_cost, rc->total);
	if (IsA(p, HashPath))
		islot_add(sl, &((HashPath *) p)->num_batches, rc->nbatches);
}

static double
loop_count(PlannerInfo *root, Path *path)
{
	return get_loop_count(root, path->parent->relid, PATH_REQ_OUTER(path));
}

/* The multipliers of the index paths of a bitmap tree */
static void
tree_mults(Replay *R, Path *node)
{
	ListCell   *lc;

	if (IsA(node, IndexPath))
	{
		TreeMult   *tm = palloc(sizeof(TreeMult));

		tm->ipath = (IndexPath *) node;
		tm->mult = touched_mult(R->root, index_rinfos(tm->ipath),
								scan_querylets(node));
		R->tree = lappend(R->tree, tm);
	}
	else if (IsA(node, BitmapAndPath))
	{
		foreach(lc, ((BitmapAndPath *) node)->bitmapquals)
			tree_mults(R, (Path *) lfirst(lc));
	}
	else if (IsA(node, BitmapOrPath))
	{
		foreach(lc, ((BitmapOrPath *) node)->bitmapquals)
			tree_mults(R, (Path *) lfirst(lc));
	}
}

/* A bitmap tree costed in scenario s (-1: point), in the scratch context */
static Path *
replay_tree(Replay *R, Path *node, int s)
{
	ListCell   *lc;

	if (IsA(node, IndexPath))
	{
		IndexPath  *ip = palloc(sizeof(IndexPath));
		RobustVec  *m = NULL;

		memcpy(ip, node, sizeof(IndexPath));
		foreach(lc, R->tree)
		{
			TreeMult   *tm = (TreeMult *) lfirst(lc);

			if (tm->ipath == (IndexPath *) node)
				m = tm->mult;
		}
		hooks.index_mult = (m != NULL && s >= 0) ? robust_vec_at(m, s) : 1.0;
		cost_index(ip, R->root, loop_count(R->root, node), false);
		hooks.index_mult = 1.0;
		return (Path *) ip;
	}
	if (IsA(node, BitmapAndPath))
	{
		BitmapAndPath *ap = palloc(sizeof(BitmapAndPath));

		memcpy(ap, node, sizeof(BitmapAndPath));
		ap->bitmapquals = NIL;
		foreach(lc, ((BitmapAndPath *) node)->bitmapquals)
			ap->bitmapquals = lappend(ap->bitmapquals,
									  replay_tree(R, (Path *) lfirst(lc), s));
		cost_bitmap_and_node(ap, R->root);
		return (Path *) ap;
	}
	if (IsA(node, BitmapOrPath))
	{
		BitmapOrPath *op = palloc(sizeof(BitmapOrPath));

		memcpy(op, node, sizeof(BitmapOrPath));
		op->bitmapquals = NIL;
		foreach(lc, ((BitmapOrPath *) node)->bitmapquals)
			op->bitmapquals = lappend(op->bitmapquals,
									  replay_tree(R, (Path *) lfirst(lc), s));
		cost_bitmap_or_node(op, R->root);
		return (Path *) op;
	}
	elog(ERROR, "RobDP: unrecognized bitmap tree node: %d", (int) nodeTag(node));
	return NULL;				/* keep compiler quiet */
}

/* The number of groups of a grouping path, sub its input, as R->groups says */
static double
groups_at(Replay *R, Path *sub)
{
	switch (R->groups)
	{
		case ROBUST_GROUPS_EST_SUB:
			return robust_replay_num_groups(R->root, R->group_exprs, sub->rows,
											NULL);
		case ROBUST_GROUPS_EST_INPUT:
			return robust_replay_num_groups(R->root, R->group_exprs,
											R->input_rel->rows, NULL);
		case ROBUST_GROUPS_SUB_ROWS:
			return sub->rows;
		case ROBUST_GROUPS_INPUT_ROWS:
			return R->input_rel->rows;
		case ROBUST_GROUPS_FROZEN:
		case ROBUST_GROUPS_NONE:
			break;
	}
	return R->frozen_groups;
}

/* Add the evaluation of the path's target per output row (create_agg_path) */
static void
add_target_cost(Path *t)
{
	t->startup_cost += t->pathtarget->cost.startup;
	t->total_cost += t->pathtarget->cost.startup +
		t->pathtarget->cost.per_tuple * t->rows;
}

/*
 * The cost functions of the path type, on t (a copy of R->path), in
 * scenario s (-1: the point).  The inputs are already set.
 */
static void
cost_copy(Replay *R, Path *t, int s)
{
	PlannerInfo *root = R->root;
	RelOptInfo *rel = t->parent;

	switch (nodeTag(t))
	{
		case T_Path:
			switch (t->pathtype)
			{
				case T_SeqScan:
					cost_seqscan(t, root, rel, t->param_info);
					break;
				case T_SampleScan:
					cost_samplescan(t, root, rel, t->param_info);
					break;
				case T_FunctionScan:
					cost_functionscan(t, root, rel, t->param_info);
					break;
				case T_TableFuncScan:
					cost_tablefuncscan(t, root, rel, t->param_info);
					break;
				case T_ValuesScan:
					cost_valuesscan(t, root, rel, t->param_info);
					break;
				case T_CteScan:
				case T_WorkTableScan:
					cost_ctescan(t, root, rel, t->param_info);
					break;
				case T_NamedTuplestoreScan:
					cost_namedtuplestorescan(t, root, rel, t->param_info);
					break;
				case T_Result:
					cost_resultscan(t, root, rel, t->param_info);
					break;
				default:
					elog(ERROR, "RobDP: no replay for path type %d",
						 (int) t->pathtype);
			}
			break;
		case T_IndexPath:
			hooks.index_mult = (R->index_mult != NULL && s >= 0) ?
				robust_vec_at(R->index_mult, s) : 1.0;
			cost_index((IndexPath *) t, root, loop_count(root, t),
					   R->path->parallel_aware);
			hooks.index_mult = 1.0;
			break;
		case T_BitmapHeapPath:
			{
				BitmapHeapPath *bp = (BitmapHeapPath *) t;

				bp->bitmapqual = replay_tree(R, bp->bitmapqual, s);
				cost_bitmap_heap_scan(t, root, rel, t->param_info,
									  bp->bitmapqual,
									  R->loop_one ? 1.0 : loop_count(root, t));
			}
			break;
		case T_TidPath:
			cost_tidscan(t, root, rel, ((TidPath *) t)->tidquals,
						 t->param_info);
			break;
		case T_TidRangePath:
			cost_tidrangescan(t, root, rel, ((TidRangePath *) t)->tidrangequals,
							  t->param_info);
			break;
		case T_SubqueryScanPath:
			cost_subqueryscan((SubqueryScanPath *) t, root, rel, t->param_info,
							  R->trivial);
			break;
		case T_AppendPath:
			{
				AppendPath *ap = (AppendPath *) t;

				if (list_length(ap->subpaths) == 1)
				{
					Path	   *child = (Path *) linitial(ap->subpaths);

					if (child->parallel_aware == t->parallel_aware)
					{
						t->rows = child->rows;
						t->startup_cost = child->startup_cost;
						t->total_cost = child->total_cost;
					}
					else
					{
						t->pathkeys = NIL;
						cost_append(ap);
					}
				}
				else
					cost_append(ap);
				{
					RobustVec  *given;

					if (append_given_rows(R->path, &given))
						t->rows = (given != NULL && s >= 0) ?
							robust_vec_at(given, s) : R->path->rows;
				}
			}
			break;
		case T_MergeAppendPath:
			{
				MergeAppendPath *mp = (MergeAppendPath *) t;
				Cost		input_startup_cost = 0;
				Cost		input_total_cost = 0;
				ListCell   *lc;

				t->rows = 0;
				foreach(lc, mp->subpaths)
				{
					Path	   *subpath = (Path *) lfirst(lc);

					t->rows += subpath->rows;
					if (pathkeys_contained_in(t->pathkeys, subpath->pathkeys))
					{
						input_startup_cost += subpath->startup_cost;
						input_total_cost += subpath->total_cost;
					}
					else
					{
						Path		sort_path;

						cost_sort(&sort_path, root, t->pathkeys,
								  subpath->total_cost, subpath->parent->tuples,
								  subpath->pathtarget->width, 0.0, work_mem,
								  mp->limit_tuples);
						input_startup_cost += sort_path.startup_cost;
						input_total_cost += sort_path.total_cost;
					}
				}
				if (list_length(mp->subpaths) == 1 &&
					((Path *) linitial(mp->subpaths))->parallel_aware ==
					t->parallel_aware)
				{
					t->startup_cost = input_startup_cost;
					t->total_cost = input_total_cost;
				}
				else
					cost_merge_append(t, root, t->pathkeys,
									  list_length(mp->subpaths),
									  input_startup_cost, input_total_cost,
									  t->rows);
			}
			break;
		case T_MaterialPath:
			{
				Path	   *sub = ((MaterialPath *) t)->subpath;

				cost_material(t, sub->startup_cost, sub->total_cost, sub->rows,
							  sub->pathtarget->width);
			}
			break;
		case T_MemoizePath:
			{
				Path	   *sub = ((MemoizePath *) t)->subpath;

				t->startup_cost = sub->startup_cost + cpu_tuple_cost;
				t->total_cost = sub->total_cost + cpu_tuple_cost;
				t->rows = sub->rows;
			}
			break;
		case T_UniquePath:
			{
				UniquePath *u = (UniquePath *) t;
				Path	   *sub = u->subpath;

				if (u->umethod == UNIQUE_PATH_NOOP)
				{
					t->rows = rel->rows;
					t->startup_cost = sub->startup_cost;
					t->total_cost = sub->total_cost;
				}
				else
				{
					int			numCols = list_length(u->uniq_exprs);

					t->rows = estimate_num_groups(root, u->uniq_exprs,
												  rel->rows, NULL, NULL);
					if (u->umethod == UNIQUE_PATH_HASH)
					{
						Path		agg_path;

						cost_agg(&agg_path, root, AGG_HASHED, NULL,
								 numCols, t->rows, NIL,
								 sub->startup_cost, sub->total_cost,
								 rel->rows, sub->pathtarget->width);
						t->startup_cost = agg_path.startup_cost;
						t->total_cost = agg_path.total_cost;
					}
					else
					{
						Path		sort_path;

						cost_sort(&sort_path, root, NIL, sub->total_cost,
								  rel->rows, sub->pathtarget->width, 0.0,
								  work_mem, -1.0);
						sort_path.total_cost += cpu_operator_cost * rel->rows * numCols;
						t->startup_cost = sort_path.startup_cost;
						t->total_cost = sort_path.total_cost;
					}
				}
			}
			break;
		case T_GatherPath:
			{
				Path	   *sub = ((GatherPath *) t)->subpath;
				double		rows = sub->rows * sub->parallel_workers;

				cost_gather((GatherPath *) t, root, rel, t->param_info,
							R->gather_rows ? &rows : NULL);
			}
			break;
		case T_GatherMergePath:
			{
				GatherMergePath *gm = (GatherMergePath *) t;
				Path	   *sub = gm->subpath;
				Cost		input_startup_cost = 0;
				Cost		input_total_cost = 0;

				if (pathkeys_contained_in(t->pathkeys, sub->pathkeys))
				{
					input_startup_cost += sub->startup_cost;
					input_total_cost += sub->total_cost;
				}
				else
				{
					Path		sort_path;

					cost_sort(&sort_path, root, t->pathkeys, sub->total_cost,
							  sub->rows, sub->pathtarget->width, 0.0,
							  work_mem, -1);
					input_startup_cost += sort_path.startup_cost;
					input_total_cost += sort_path.total_cost;
				}
				{
					double		rows = sub->rows * sub->parallel_workers;

					cost_gather_merge(gm, root, rel, t->param_info,
									  input_startup_cost, input_total_cost,
									  R->gather_rows ? &rows : NULL);
				}
			}
			break;
		case T_SortPath:
			{
				Path	   *sub = ((SortPath *) t)->subpath;

				cost_sort(t, root, t->pathkeys, sub->total_cost, sub->rows,
						  sub->pathtarget->width, 0.0, work_mem,
						  R->limit_tuples);
			}
			break;
		case T_IncrementalSortPath:
			{
				IncrementalSortPath *ip = (IncrementalSortPath *) t;
				Path	   *sub = ip->spath.subpath;

				cost_incremental_sort(t, root, t->pathkeys, ip->nPresortedCols,
									  sub->startup_cost, sub->total_cost,
									  sub->rows, sub->pathtarget->width, 0.0,
									  work_mem, R->limit_tuples);
			}
			break;
		case T_ProjectSetPath:
			{
				/* create_set_projection_path */
				Path	   *sub = ((ProjectSetPath *) t)->subpath;
				PathTarget *target = t->pathtarget;
				double		tlist_rows = 1;
				ListCell   *lc;

				foreach(lc, target->exprs)
				{
					double		itemrows = expression_returns_set_rows(root,
																	   (Node *) lfirst(lc));

					if (tlist_rows < itemrows)
						tlist_rows = itemrows;
				}
				t->rows = sub->rows * tlist_rows;
				t->startup_cost = sub->startup_cost + target->cost.startup;
				t->total_cost = sub->total_cost +
					target->cost.startup +
					(cpu_tuple_cost + target->cost.per_tuple) * sub->rows +
					(t->rows - sub->rows) * cpu_tuple_cost / 2;
			}
			break;
		case T_ProjectionPath:
			{
				ProjectionPath *pp = (ProjectionPath *) t;
				Path	   *sub = pp->subpath;
				PathTarget *target = t->pathtarget;
				PathTarget *oldtarget = sub->pathtarget;

				t->rows = sub->rows;
				if (pp->dummypp)
				{
					t->startup_cost = sub->startup_cost +
						(target->cost.startup - oldtarget->cost.startup);
					t->total_cost = sub->total_cost +
						(target->cost.startup - oldtarget->cost.startup) +
						(target->cost.per_tuple - oldtarget->cost.per_tuple) * sub->rows;
				}
				else
				{
					t->startup_cost = sub->startup_cost + target->cost.startup;
					t->total_cost = sub->total_cost + target->cost.startup +
						(cpu_tuple_cost + target->cost.per_tuple) * sub->rows;
				}
			}
			break;
		case T_NestPath:
		case T_MergePath:
		case T_HashPath:
			{
				JoinPath   *jp = (JoinPath *) t;
				JoinPathExtraData extra;
				JoinCostWorkspace ws;

				memset(&extra, 0, sizeof(extra));
				extra.restrictlist = jcx.restrictlist;
				extra.sjinfo = jcx.sjinfo;
				extra.inner_unique = jcx.inner_unique;
				if (jcx.has_semi)
					extra.semifactors = (s >= 0 && jcx.sf != NULL) ?
						jcx.sf[s] : jcx.semifactors;
				hooks.quals = R->quals;
				hooks.qual_mult = (R->qual_mult != NULL && s >= 0) ?
					robust_vec_at(R->qual_mult, s) : 1.0;

				if (IsA(t, NestPath))
				{
					initial_cost_nestloop(root, &ws, jp->jointype,
										  jp->outerjoinpath, jp->innerjoinpath,
										  &extra);
					final_cost_nestloop(root, (NestPath *) t, &ws, &extra);
				}
				else if (IsA(t, MergePath))
				{
					MergePath  *mp = (MergePath *) t;

					initial_cost_mergejoin(root, &ws, jp->jointype,
										   mp->path_mergeclauses,
										   jp->outerjoinpath, jp->innerjoinpath,
										   mp->outersortkeys, mp->innersortkeys,
										   &extra);
					final_cost_mergejoin(root, mp, &ws, &extra);
				}
				else
				{
					HashPath   *hp = (HashPath *) t;

					initial_cost_hashjoin(root, &ws, jp->jointype,
										  hp->path_hashclauses,
										  jp->outerjoinpath, jp->innerjoinpath,
										  &extra, t->parallel_aware);
					final_cost_hashjoin(root, hp, &ws, &extra);
				}
				hooks.quals = NIL;
				hooks.qual_mult = 1.0;
			}
			break;

			/* Upper paths: as their create_*_path functions cost them */
		case T_AggPath:
			{
				AggPath    *ap = (AggPath *) t;
				Path	   *sub = ap->subpath;
				double		ng = groups_at(R, sub);

				ap->numGroups = ng;
				cost_agg(t, root, ap->aggstrategy,
						 R->upper->has_aggcosts ? &R->upper->aggcosts : NULL,
						 list_length(ap->groupClause), ng, ap->qual,
						 sub->startup_cost, sub->total_cost, sub->rows,
						 sub->pathtarget->width);
				add_target_cost(t);
			}
			break;
		case T_GroupPath:
			{
				GroupPath  *gp = (GroupPath *) t;
				Path	   *sub = gp->subpath;

				cost_group(t, root, list_length(gp->groupClause),
						   groups_at(R, sub), gp->qual,
						   sub->startup_cost, sub->total_cost, sub->rows);
				add_target_cost(t);
			}
			break;
		case T_UpperUniquePath:
			{
				UpperUniquePath *up = (UpperUniquePath *) t;
				Path	   *sub = up->subpath;

				t->startup_cost = sub->startup_cost;
				t->total_cost = sub->total_cost +
					cpu_operator_cost * sub->rows * up->numkeys;
				t->rows = groups_at(R, sub);
			}
			break;
		case T_WindowAggPath:
			{
				WindowAggPath *wp = (WindowAggPath *) t;
				Path	   *sub = wp->subpath;

				cost_windowagg(t, root, R->upper->window_funcs,
							   list_length(wp->winclause->partitionClause),
							   list_length(wp->winclause->orderClause),
							   sub->startup_cost, sub->total_cost, sub->rows);
				add_target_cost(t);
			}
			break;
		case T_SetOpPath:
			{
				SetOpPath  *sp = (SetOpPath *) t;
				Path	   *sub = sp->subpath;

				t->startup_cost = sub->startup_cost;
				t->total_cost = sub->total_cost +
					cpu_operator_cost * sub->rows * list_length(sp->distinctList);
				t->rows = R->frozen_groups;
			}
			break;
		case T_LockRowsPath:
			{
				Path	   *sub = ((LockRowsPath *) t)->subpath;

				t->rows = sub->rows;
				t->startup_cost = sub->startup_cost;
				t->total_cost = sub->total_cost + cpu_tuple_cost * sub->rows;
			}
			break;
		case T_ModifyTablePath:
			{
				ModifyTablePath *mp = (ModifyTablePath *) t;
				Path	   *sub = mp->subpath;

				t->startup_cost = sub->startup_cost;
				t->total_cost = sub->total_cost;
				t->rows = mp->returningLists != NIL ? sub->rows : 0;
			}
			break;
		case T_LimitPath:
			{
				Path	   *sub = ((LimitPath *) t)->subpath;

				t->rows = sub->rows;
				t->startup_cost = sub->startup_cost;
				t->total_cost = sub->total_cost;
				adjust_limit_rows_costs(&t->rows, &t->startup_cost,
										&t->total_cost, R->upper->offset_est,
										R->upper->count_est);
			}
			break;
		default:
			elog(ERROR, "RobDP: no replay for path node %d", (int) nodeTag(t));
	}
}

/* Replay once, in scenario s (-1: the point, nothing set) */
static void
replay_once(Replay *R, int s, double *rows, double *startup, double *total,
			int *nbatches)
{
	Path	   *t = R->buf;
	MemoryContext oldcxt = MemoryContextSwitchTo(scratch_cxt);

	memcpy(t, R->path, R->size);
	replay_path = R->path;
	replay_s = s;
	if (s >= 0)
		slots_project(&R->slots, s);
	hooks.index_mult = 1.0;
	hooks.quals = NIL;
	hooks.qual_mult = 1.0;
	hooks.semi_mult = 1.0;
	robust_replay = &hooks;
	cost_copy(R, t, s);
	robust_replay = NULL;
	replay_path = NULL;
	replay_s = -1;
	if (s >= 0)
		slots_restore(&R->slots);
	MemoryContextSwitchTo(oldcxt);
	MemoryContextReset(scratch_cxt);

	*rows = t->rows;
	*startup = t->startup_cost;
	*total = t->total_cost;
	*nbatches = IsA(t, HashPath) ? ((HashPath *) t)->num_batches : 0;
}

/* Does the replay with nothing changed reproduce the path's scalars? */
static bool
reproduces(Replay *R)
{
	double		rows,
				startup,
				total;
	int			nb;

	replay_once(R, -1, &rows, &startup, &total, &nb);
	R->got[0] = rows;
	R->got[1] = startup;
	R->got[2] = total;
	return rows == R->path->rows &&
		startup == R->path->startup_cost &&
		total == R->path->total_cost &&
		(!IsA(R->path, HashPath) || nb == ((HashPath *) R->path)->num_batches);
}

/* A vector, or NULL when it equals the scalar in every scenario */
static RobustVec *
vec_or_null(RobustVec *v, double scalar)
{
	for (int s = 0; s < v->n; s++)
		if (v->v[s] != scalar)
			return v;
	pfree(v);
	return NULL;
}

/* Is rel the upper relation of the kind? */
static bool
is_upper_rel(PlannerInfo *root, RelOptInfo *rel, UpperRelationKind kind)
{
	ListCell   *lc;

	foreach(lc, root->upper_rels[kind])
	{
		if (lfirst(lc) == rel)
			return true;
	}
	return false;
}

/*
 * The grouping expressions of clauses (SortGroupClauses), as the input's
 * target has them; false if one is missing.
 */
static bool
group_exprs(List *clauses, PathTarget *target, List **exprs)
{
	ListCell   *lc;

	*exprs = NIL;
	foreach(lc, clauses)
	{
		SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
		Expr	   *e = NULL;
		ListCell   *lc2;

		if (target->sortgrouprefs == NULL)
			return false;
		foreach(lc2, target->exprs)
		{
			if (target->sortgrouprefs[foreach_current_index(lc2)] ==
				sgc->tleSortGroupRef)
			{
				e = (Expr *) lfirst(lc2);
				break;
			}
		}
		if (e == NULL)
			return false;
		*exprs = lappend(*exprs, e);
	}
	return true;
}

/*
 * Can estimate_num_groups take the expressions?  Their Vars must belong to
 * relations of this query level (those of a set operation's output do not).
 */
static bool
exprs_estimable(PlannerInfo *root, List *exprs)
{
	List	   *vars = pull_var_clause((Node *) exprs,
									   PVC_RECURSE_AGGREGATES |
									   PVC_RECURSE_WINDOWFUNCS |
									   PVC_RECURSE_PLACEHOLDERS);
	ListCell   *lc;

	foreach(lc, vars)
	{
		Var		   *v = (Var *) lfirst(lc);

		if (!IsA(v, Var) || v->varno < 1 ||
			v->varno >= root->simple_rel_array_size ||
			root->simple_rel_array[v->varno] == NULL)
			return false;
	}
	return true;
}

/*
 * The relation a grouping reads: down from its input path through the
 * paths of upper relations (a Sort, the Gather of a partial aggregation and
 * the partial aggregation itself) to the first path of another relation.
 */
static RelOptInfo *
grouping_input_rel(Path *path)
{
	while (path->parent->reloptkind == RELOPT_UPPER_REL ||
		   path->parent->reloptkind == RELOPT_OTHER_UPPER_REL)
	{
		List	   *children = path_children(path);

		if (list_length(children) != 1)
			return NULL;
		path = (Path *) linitial(children);
	}
	return path->parent;
}

/* What a grouping path's number of groups can be got from */
static void
groups_setup(Replay *R, Path *path)
{
	PlannerInfo *root = R->root;
	Path	   *sub = (Path *) linitial(path_children(path));
	List	   *clauses = NIL;
	bool		exprs = false;

	switch (nodeTag(path))
	{
		case T_AggPath:
			clauses = ((AggPath *) path)->groupClause;
			exprs = true;
			R->frozen_groups = ((AggPath *) path)->numGroups;
			R->have_frozen = true;
			break;
		case T_GroupPath:
			clauses = ((GroupPath *) path)->groupClause;
			exprs = true;
			/* with a HAVING qual, the groups are not the rows */
			if (((GroupPath *) path)->qual == NIL)
			{
				R->frozen_groups = path->rows;
				R->have_frozen = true;
			}
			break;
		case T_UpperUniquePath:
			/* a DISTINCT's (a set operation's has its input rows) */
			if (is_upper_rel(root, path->parent, UPPERREL_DISTINCT) ||
				is_upper_rel(root, path->parent, UPPERREL_PARTIAL_DISTINCT))
			{
				clauses = root->processed_distinctClause;
				exprs = true;
			}
			R->frozen_groups = path->rows;
			R->have_frozen = true;
			break;
		case T_SetOpPath:
			/* rows from the inputs' groups, estimated by prepunion.c */
			R->frozen_groups = path->rows;
			R->have_frozen = true;
			R->groups = ROBUST_GROUPS_FROZEN;
			return;
		default:
			return;
	}
	if (exprs)
		R->have_exprs = group_exprs(clauses, sub->pathtarget, &R->group_exprs) &&
			exprs_estimable(root, R->group_exprs);
	R->input_rel = grouping_input_rel(sub);
}

static bool
groups_available(Replay *R, RobustGroups g)
{
	switch (g)
	{
		case ROBUST_GROUPS_EST_SUB:
			return R->have_exprs;
		case ROBUST_GROUPS_EST_INPUT:
			return R->have_exprs && R->input_rel != NULL;
		case ROBUST_GROUPS_SUB_ROWS:
			return true;
		case ROBUST_GROUPS_INPUT_ROWS:
			return R->input_rel != NULL;
		case ROBUST_GROUPS_FROZEN:
			return R->have_frozen;
		case ROBUST_GROUPS_NONE:
			break;
	}
	return false;
}

static void
replay(PlannerInfo *root, Path *path, RobustPathCost *rc)
{
	Replay		R;
	List	   *children = path_children(path);
	bool		varies = false;
	ListCell   *lc;
	int			N = error_sample_count;
	RobustVec  *rows,
			   *startup,
			   *total;
	int		   *nb = NULL;
	MemoryContext pathcxt = GetMemoryChunkContext(path);
	bool		round = root->robust_pass == ROBUST_PASS_ENVELOPE;

	memset(&R, 0, sizeof(R));
	R.root = root;
	R.path = path;
	R.size = path_size(path);
	R.limit_tuples = -1.0;

	/*
	 * The inputs first (those of a partial path, or of a Gather, are partial:
	 * they matter when an input was put in its pathlist without add_path)
	 */
	foreach(lc, children)
		varies |= cost_varies(compute(root, (Path *) lfirst(lc),
									  rc->partial || IsA(path, GatherPath) ||
									  IsA(path, GatherMergePath)));
	if (IsA(path, SubqueryScanPath))
		varies |= cost_varies(((SubqueryScanPath *) path)->subpath->robust);

	varies |= path->parent->robust_rows != NULL;
	varies |= path->param_info != NULL &&
		path->param_info->ppi_robust_rows != NULL;
	varies |= base_rels_vary(root, path->parent->relids);
	varies |= base_rels_vary(root, PATH_REQ_OUTER(path));

	/*
	 * An envelope round's paths have the round's values in their scalar
	 * fields and no vectors; still, the round's scenario may scale the
	 * selectivities the path's own cost function estimates.
	 */
	if (round)
		varies = st->varies;
	if (!varies)
	{
		rc->status = ROBUST_COST_CONST;
		return;
	}
	if (R.size == 0)
	{
		rc->status = ROBUST_COST_UNSUPPORTED;
		return;
	}

	R.join = IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath);
	if (R.join)
	{
		JoinPath   *jp = (JoinPath *) path;

		if (!jcx.valid || jcx.root != root || jcx.joinrel != path->parent ||
			jp->outerjoinpath->parent != jcx.outerrel ||
			jp->innerjoinpath->parent != jcx.innerrel)
		{
			rc->status = ROBUST_COST_NOCONTEXT;
			return;
		}
		if (jcx.has_semi && jcx.sf == NULL)
			compute_semifactors();
		rc->semi_frozen = jcx.sf_frozen;
		if (IsA(path, MergePath))
			R.quals = ((MergePath *) path)->path_mergeclauses;
		else if (IsA(path, HashPath))
			R.quals = ((HashPath *) path)->path_hashclauses;
		if (R.quals != NIL)
			R.qual_mult = touched_mult(root, R.quals,
									   join_path_querylets(root, jp, jcx.sjinfo));
	}
	else if (IsA(path, IndexPath))
		R.index_mult = touched_mult(root, index_rinfos((IndexPath *) path),
									scan_querylets(path));
	else if (IsA(path, BitmapHeapPath))
		tree_mults(&R, ((BitmapHeapPath *) path)->bitmapqual);

	/* Upper paths: what their constructors were given */
	if (IsA(path, AggPath) || IsA(path, WindowAggPath) || IsA(path, LimitPath))
	{
		R.upper = upper_info(path, false);
		if (R.upper == NULL)
		{
			rc->status = ROBUST_COST_NOCONTEXT;
			return;
		}
	}
	if (IsA(path, AggPath) || IsA(path, GroupPath) ||
		IsA(path, UpperUniquePath) || IsA(path, SetOpPath))
		groups_setup(&R, path);

	/* The inputs set per scenario */
	add_base_rels(&R.slots, root);
	add_rel(&R.slots, path->parent);
	if (path->param_info != NULL)
		slot_add(&R.slots, &path->param_info->ppi_rows,
				 path->param_info->ppi_robust_rows);
	foreach(lc, children)
		add_path_slots(&R.slots, (Path *) lfirst(lc));
	if (IsA(path, SubqueryScanPath))
		add_path_slots(&R.slots, ((SubqueryScanPath *) path)->subpath);
	if (R.input_rel != NULL)
		add_rel(&R.slots, R.input_rel);
	if (IsA(path, NestPath) && IsA(((JoinPath *) path)->innerjoinpath, MemoizePath))
	{
		/* cost_memoize_rescan reads the Memoize's input and calls */
		JoinPath   *jp = (JoinPath *) path;
		MemoizePath *mp = (MemoizePath *) jp->innerjoinpath;
		RobustPathCost *orc = jp->outerjoinpath->robust;

		add_path_slots(&R.slots, mp->subpath);
		if (orc != NULL && orc->rows != NULL &&
			mp->calls == clamp_row_est(jp->outerjoinpath->rows))
		{
			RobustVec  *calls = robust_vec_make(N);

			for (int s = 0; s < N; s++)
				calls->v[s] = clamp_row_est(robust_vec_at(orc->rows, s));
			slot_add(&R.slots, &mp->calls, calls);
		}
		R.memo = mp;
		R.memo_entries = mp->est_entries;
		R.memo_exprs = mp->param_exprs;
		mp->param_exprs = memo_costed_exprs(mp);
	}

	R.buf = palloc(R.size);

	/* The point first, trying the variants the path does not record */
	{
		bool		ok;

		if (R.groups == ROBUST_GROUPS_NONE &&
			(IsA(path, AggPath) || IsA(path, GroupPath) ||
			 IsA(path, UpperUniquePath)))
		{
			/* the first source of the groups that gives the point */
			ok = false;
			for (int g = ROBUST_GROUPS_EST_SUB; g <= ROBUST_GROUPS_FROZEN && !ok; g++)
			{
				if (!groups_available(&R, (RobustGroups) g))
					continue;
				R.groups = (RobustGroups) g;
				ok = reproduces(&R);
			}
		}
		else
			ok = reproduces(&R);
		rc->groups = ok ? R.groups : ROBUST_GROUPS_NONE;

		if (!ok && (IsA(path, SortPath) || IsA(path, IncrementalSortPath)))
		{
			R.limit_tuples = root->limit_tuples;
			ok = reproduces(&R);
			if (!ok && is_upper_rel(root, path->parent, UPPERREL_ORDERED) &&
				ordered_limit(root, &R.limit_tuples))
				ok = reproduces(&R);
		}
		if (!ok && IsA(path, SubqueryScanPath))
		{
			R.trivial = true;
			ok = reproduces(&R);
		}
		if (!ok && (IsA(path, GatherPath) || IsA(path, GatherMergePath)))
		{
			/* generate_gather_paths(override_rows), gather_grouping_paths */
			R.gather_rows = true;
			ok = reproduces(&R);
		}
		if (!ok && IsA(path, BitmapHeapPath) &&
			bms_equal(PATH_REQ_OUTER(path), path->parent->lateral_relids))
		{
			R.loop_one = true;
			ok = reproduces(&R);
		}
		if (!ok)
		{
			if (R.memo)
			{
				R.memo->est_entries = R.memo_entries;
				R.memo->param_exprs = R.memo_exprs;
			}
			rc->status = ROBUST_COST_MISMATCH;
			memcpy(rc->replayed, R.got, sizeof(rc->replayed));
			return;
		}
	}

	/* An envelope round: the round's scenario only, into the scalars */
	if (round)
	{
		double		r,
					su,
					t;
		int			b;

		replay_once(&R, root->robust_round, &r, &su, &t, &b);
		if (R.memo)
		{
			R.memo->est_entries = R.memo_entries;
			R.memo->param_exprs = R.memo_exprs;
		}
		path->rows = r;
		path->startup_cost = su;
		path->total_cost = t;
		if (IsA(path, HashPath))
			((HashPath *) path)->num_batches = b;
		rc->status = ROBUST_COST_REPLAY;
		pfree(R.buf);
		return;
	}

	rows = MemoryContextAlloc(pathcxt, offsetof(RobustVec, v) + sizeof(double) * N);
	startup = MemoryContextAlloc(pathcxt, offsetof(RobustVec, v) + sizeof(double) * N);
	total = MemoryContextAlloc(pathcxt, offsetof(RobustVec, v) + sizeof(double) * N);
	rows->n = startup->n = total->n = N;
	if (IsA(path, HashPath))
		nb = MemoryContextAlloc(pathcxt, sizeof(int) * N);
	for (int s = 0; s < N; s++)
	{
		int			b;

		replay_once(&R, s, &rows->v[s], &startup->v[s], &total->v[s], &b);
		if (nb)
			nb[s] = b;
	}
	if (R.memo)
	{
		R.memo->est_entries = R.memo_entries;
		R.memo->param_exprs = R.memo_exprs;
	}

	rc->status = ROBUST_COST_REPLAY;
	rc->rows = vec_or_null(rows, path->rows);
	rc->startup = vec_or_null(startup, path->startup_cost);
	rc->total = vec_or_null(total, path->total_cost);
	if (nb)
	{
		for (int s = 0; s < N; s++)
			if (nb[s] != ((HashPath *) path)->num_batches)
			{
				rc->nbatches = nb;
				break;
			}
		if (rc->nbatches == NULL)
			pfree(nb);
	}
	pfree(R.buf);
}

/* ------------------------------------------------------------- dumping */

static const char *
path_type_name(Path *path)
{
	switch (nodeTag(path))
	{
		case T_Path:
			switch (path->pathtype)
			{
				case T_SeqScan:
					return "SeqScan";
				case T_SampleScan:
					return "SampleScan";
				case T_FunctionScan:
					return "FunctionScan";
				case T_TableFuncScan:
					return "TableFuncScan";
				case T_ValuesScan:
					return "ValuesScan";
				case T_CteScan:
					return "CteScan";
				case T_WorkTableScan:
					return "WorkTableScan";
				case T_NamedTuplestoreScan:
					return "NamedTuplestoreScan";
				case T_Result:
					return "Result";
				default:
					return "Path";
			}
		case T_IndexPath:
			return path->pathtype == T_IndexOnlyScan ? "IndexOnlyScan" : "IndexScan";
		case T_BitmapHeapPath:
			return "BitmapHeapScan";
		case T_TidPath:
			return "TidScan";
		case T_TidRangePath:
			return "TidRangeScan";
		case T_SubqueryScanPath:
			return "SubqueryScan";
		case T_ForeignPath:
			return "ForeignScan";
		case T_CustomPath:
			return "CustomScan";
		case T_AppendPath:
			return "Append";
		case T_MergeAppendPath:
			return "MergeAppend";
		case T_GroupResultPath:
			return "GroupResult";
		case T_MaterialPath:
			return "Material";
		case T_MemoizePath:
			return "Memoize";
		case T_UniquePath:
			return "Unique";
		case T_GatherPath:
			return "Gather";
		case T_GatherMergePath:
			return "GatherMerge";
		case T_NestPath:
			return "NestLoop";
		case T_MergePath:
			return "MergeJoin";
		case T_HashPath:
			return "HashJoin";
		case T_ProjectionPath:
			return "Projection";
		case T_ProjectSetPath:
			return "ProjectSet";
		case T_SortPath:
			return "Sort";
		case T_IncrementalSortPath:
			return "IncrementalSort";
		case T_AggPath:
			return "Agg";
		case T_GroupPath:
			return "Group";
		case T_UpperUniquePath:
			return "UpperUnique";
		case T_WindowAggPath:
			return "WindowAgg";
		case T_SetOpPath:
			return "SetOp";
		case T_LockRowsPath:
			return "LockRows";
		case T_ModifyTablePath:
			return "ModifyTable";
		case T_LimitPath:
			return "Limit";
		case T_GroupingSetsPath:
			return "GroupingSets";
		case T_RecursiveUnionPath:
			return "RecursiveUnion";
		case T_MinMaxAggPath:
			return "MinMaxAgg";
		default:
			return psprintf("Other(tag %d, pathtype %d)", (int) nodeTag(path), (int) path->pathtype);
	}
}

static const char *
status_name(RobustCostStatus st)
{
	switch (st)
	{
		case ROBUST_COST_CONST:
			return "const";
		case ROBUST_COST_REPLAY:
			return "replay";
		case ROBUST_COST_MISMATCH:
			return "mismatch";
		case ROBUST_COST_UNSUPPORTED:
			return "unsupported";
		case ROBUST_COST_NOCONTEXT:
			return "nocontext";
	}
	return "?";
}

static const char *
jointype_str(JoinType jt)
{
	switch (jt)
	{
		case JOIN_INNER:
			return "inner";
		case JOIN_LEFT:
			return "left";
		case JOIN_FULL:
			return "full";
		case JOIN_RIGHT:
			return "right";
		case JOIN_SEMI:
			return "semi";
		case JOIN_ANTI:
			return "anti";
		case JOIN_RIGHT_ANTI:
			return "right_anti";
		default:
			return "other";
	}
}

static void
tree_detail(StringInfo buf, Path *node)
{
	ListCell   *lc;

	if (IsA(node, IndexPath))
	{
		char	   *name = get_rel_name(((IndexPath *) node)->indexinfo->indexoid);

		appendStringInfoString(buf, name ? name : "?");
		return;
	}
	appendStringInfoString(buf, IsA(node, BitmapAndPath) ? "and(" : "or(");
	foreach(lc, IsA(node, BitmapAndPath) ? ((BitmapAndPath *) node)->bitmapquals :
			((BitmapOrPath *) node)->bitmapquals)
	{
		if (foreach_current_index(lc) > 0)
			appendStringInfoChar(buf, ',');
		tree_detail(buf, (Path *) lfirst(lc));
	}
	appendStringInfoChar(buf, ')');
}

/* What tells paths of the same type, relation and inputs apart */
static char *
path_detail(Path *path)
{
	StringInfoData buf;

	initStringInfo(&buf);
	switch (nodeTag(path))
	{
		case T_IndexPath:
			tree_detail(&buf, path);
			break;
		case T_BitmapHeapPath:
			tree_detail(&buf, ((BitmapHeapPath *) path)->bitmapqual);
			break;
		case T_NestPath:
			appendStringInfoString(&buf, jointype_str(((JoinPath *) path)->jointype));
			break;
		case T_MergePath:
			{
				MergePath  *mp = (MergePath *) path;

				appendStringInfo(&buf, "%s/%d/%d/%d%s",
								 jointype_str(mp->jpath.jointype),
								 list_length(mp->path_mergeclauses),
								 list_length(mp->outersortkeys),
								 list_length(mp->innersortkeys),
								 mp->materialize_inner ? "/mat" : "");
			}
			break;
		case T_HashPath:
			appendStringInfo(&buf, "%s/%d",
							 jointype_str(((JoinPath *) path)->jointype),
							 list_length(((HashPath *) path)->path_hashclauses));
			break;
		case T_UniquePath:
			appendStringInfo(&buf, "%d", (int) ((UniquePath *) path)->umethod);
			break;
		case T_IncrementalSortPath:
			appendStringInfo(&buf, "%d", ((IncrementalSortPath *) path)->nPresortedCols);
			break;
		case T_GatherPath:
			appendStringInfo(&buf, "%d", ((GatherPath *) path)->num_workers);
			break;
		case T_GatherMergePath:
			appendStringInfo(&buf, "%d", ((GatherMergePath *) path)->num_workers);
			break;
		case T_AggPath:
			{
				static const char *const strategy[] = {"plain", "sorted", "hashed", "mixed"};
				AggPath    *ap = (AggPath *) path;

				appendStringInfo(&buf, "%s/%s", strategy[ap->aggstrategy],
								 ap->aggsplit == AGGSPLIT_SIMPLE ? "simple" :
								 ap->aggsplit == AGGSPLIT_INITIAL_SERIAL ? "partial" :
								 ap->aggsplit == AGGSPLIT_FINAL_DESERIAL ? "final" :
								 "other");
			}
			break;
		case T_UpperUniquePath:
			appendStringInfo(&buf, "%d", ((UpperUniquePath *) path)->numkeys);
			break;
		default:
			break;
	}
	if (path->parallel_workers > 0)
		appendStringInfo(&buf, "%sw%d", buf.len > 0 ? "/" : "", path->parallel_workers);
	return buf.data;
}

static const char *
groups_name(RobustGroups g)
{
	switch (g)
	{
		case ROBUST_GROUPS_EST_SUB:
			return "est_sub";
		case ROBUST_GROUPS_EST_INPUT:
			return "est_input";
		case ROBUST_GROUPS_SUB_ROWS:
			return "sub_rows";
		case ROBUST_GROUPS_INPUT_ROWS:
			return "input_rows";
		case ROBUST_GROUPS_FROZEN:
			return "frozen";
		case ROBUST_GROUPS_NONE:
			break;
	}
	return NULL;
}

/* The kind of an upper relation, as named in the dump */
static const char *
upper_kind_name(PlannerInfo *root, RelOptInfo *rel)
{
	static const char *const names[] = {"setop", "partial_grouping",
		"grouping", "window", "partial_distinct", "distinct", "ordered",
	"final"};

	StaticAssertStmt(lengthof(names) == UPPERREL_FINAL + 1,
					 "upper relation names");
	for (int k = 0; k <= UPPERREL_FINAL; k++)
	{
		if (is_upper_rel(root, rel, (UpperRelationKind) k))
			return names[k];
	}
	return "other";
}

static void
dump_path(PlannerInfo *root, Path *path, RobustPathCost *rc)
{
	RobustDump	d;
	StringInfoData buf;
	List	   *children = path_children(path);
	ListCell   *lc;

	if (IsA(path, SubqueryScanPath))
		children = list_make1(((SubqueryScanPath *) path)->subpath);

	robust_dump_begin(&d, "path");
	robust_dump_str(&d, "pass", robust_pass_name(root->robust_pass));
	robust_dump_relids(&d, "rels", root, path->parent->relids);
	if (IS_UPPER_REL(path->parent))
		robust_dump_str(&d, "upper", upper_kind_name(root, path->parent));
	robust_dump_int(&d, "id", rc->id);
	robust_dump_str(&d, "type", path_type_name(path));
	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	foreach(lc, children)
	{
		Path	   *c = (Path *) lfirst(lc);

		if (foreach_current_index(lc) > 0)
			appendStringInfoChar(&buf, ',');
		if (c->robust != NULL)
			appendStringInfo(&buf, "%d", c->robust->id);
		else
			appendStringInfoString(&buf, "null");
	}
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(&d, "children", buf.data);
	pfree(buf.data);
	robust_dump_relids(&d, "param", root, PATH_REQ_OUTER(path));
	robust_dump_int(&d, "npathkeys", list_length(path->pathkeys));
	robust_dump_bool(&d, "partial", rc->partial);
	robust_dump_bool(&d, "parallel_safe", path->parallel_safe);
	robust_dump_str(&d, "status", status_name(rc->status));
	robust_dump_str(&d, "detail", path_detail(path));
	if (rc->semi_frozen)
		robust_dump_bool(&d, "semi_frozen", true);
	if (rc->groups != ROBUST_GROUPS_NONE)
		robust_dump_str(&d, "groups", groups_name(rc->groups));
	if (rc->status == ROBUST_COST_MISMATCH)
	{
		robust_dump_double(&d, "replayed_rows", rc->replayed[0]);
		robust_dump_double(&d, "replayed_startup", rc->replayed[1]);
		robust_dump_double(&d, "replayed_total", rc->replayed[2]);
	}
	robust_dump_double(&d, "rows_point", path->rows);
	robust_dump_double(&d, "startup_point", path->startup_cost);
	robust_dump_double(&d, "total_point", path->total_cost);
	robust_dump_vec(&d, "rows", rc->rows ? rc->rows : robust_vec_const(path->rows));
	robust_dump_vec(&d, "startup", rc->startup ? rc->startup :
					robust_vec_const(path->startup_cost));
	robust_dump_vec(&d, "total", rc->total ? rc->total :
					robust_vec_const(path->total_cost));
	robust_dump_end(&d);
}

/* --------------------------------------------------------- entry point */

static RobustPathCost *
compute(PlannerInfo *root, Path *path, bool partial)
{
	RobustPathCost *rc;

	if (path->robust != NULL)
		return path->robust;
	rc = MemoryContextAllocZero(GetMemoryChunkContext(path),
								sizeof(RobustPathCost));
	rc->id = root->robust_pass == ROBUST_PASS_ENVELOPE ? -1 :
		st->next_path_id++;
	rc->partial = partial;
	path->robust = rc;
	replay(root, path, rc);
	if (root->robust_pass == ROBUST_PASS_ENVELOPE)
		st->round_counts[rc->status]++;
	else if (cost_varies(rc))
		st->varies = true;
	if (IsA(path, HashPath))
		bucket_fills_done((HashPath *) path);
	/* (an envelope round's paths only when their replay failed) */
	if (robust_dumping(root) ||
		(robust_debug_dump && rc->status == ROBUST_COST_MISMATCH))
		dump_path(root, path, rc);
	return rc;
}

/*
 * robust_cost_add_path
 *		Called by add_path() and add_partial_path() for every path offered to
 *		a relation, of the join tree or above it: computes the path's vectors
 *		(and those of its inputs not computed yet).  Also for a path put in a
 *		pathlist without add_path, when it is compared to another.
 */
void
robust_cost_add_path(Path *path, bool partial)
{
	PlannerInfo *root = st ? st->root : NULL;

	if (root == NULL || path->parent == NULL)
		return;
	if (scratch_cxt == NULL)
		scratch_cxt = AllocSetContextCreate(TopMemoryContext,
											"RobDP replay",
											ALLOCSET_DEFAULT_SIZES);
	(void) compute(root, path, partial);
}
