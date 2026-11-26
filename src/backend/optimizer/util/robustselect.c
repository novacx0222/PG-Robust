/*-------------------------------------------------------------------------
 *
 * robustselect.c
 *	  Selection of the paths a relation of RobDP's main pass keeps.
 *
 * See optimizer/robustselect.h.  robustsearch.c calls robust_select_paths()
 * when a relation of the main pass is complete; add_path() and
 * add_partial_path() hand the candidates to robust_select_offer().
 *
 * src/backend/optimizer/util/robustselect.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "common/pg_prng.h"
#include "nodes/bitmapset.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/robust.h"
#include "optimizer/robustcost.h"
#include "optimizer/robustsearch.h"
#include "optimizer/robustselect.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/memutils.h"

/*
 * The objectives and diversification functions, in v20's numbering (see
 * optimizer/robustselect.h).  9 (v20's "jointype-based score") was never
 * implemented, in v20 either.
 */
#define OBJ_WORST_PENALTY		0
#define OBJ_EXPECTED_PENALTY	1
#define OBJ_WORST_TOTAL			2
#define OBJ_EXPECTED_TOTAL		3
#define OBJ_WORST_STARTUP		4
#define OBJ_EXPECTED_STARTUP	5
#define OBJ_PENALTY_COUNT		6	/* N (1 - ProbNoPenalty) */
#define DV_RANDOM				7
#define OBJ_POINT_TOTAL			8
#define OBJ_WORST_PENALTY_STD	10
#define OBJ_EXPECTED_PENALTY_STD 11
#define OBJ_QUANTILE_PENALTY	12
#define DV_SIMILARITY			13
#define DV_MIX					14
#define DV_PENALTY_JOIN			15
#define DV_COST_JOIN			16
#define OBJ_QUANTILE_TOTAL		17
#define OBJ_QUANTILE_STARTUP	18

/* What an objective measures in each scenario ... */
typedef enum Loss
{
	LOSS_PENALTY,				/* Penalty(s) */
	LOSS_EXCESS,				/* 1 if Penalty(s) > 0, else 0 */
	LOSS_TOTAL,					/* total(s) */
	LOSS_STARTUP,				/* startup(s) */
	LOSS_POINT,					/* PostgreSQL's estimate */
} Loss;

/* ... and how it sums the scenarios up */
typedef enum Agg
{
	AGG_MAX,					/* the largest (from 0, as v20) */
	AGG_MEAN,
	AGG_SUM,					/* the sum: a count, for 0/1 losses */
	AGG_MEAN_STD,				/* mean + lambda * standard deviation */
	AGG_MAX_STD,				/* max + lambda * standard deviation */
	AGG_QUANTILE,				/* the alpha-quantile */
} Agg;

/* Why a path was kept (the "by" of the keep event) */
typedef enum KeptBy
{
	KEPT_NOT = 0,
	KEPT_MAIN,
	KEPT_RETAIN,
	KEPT_UNPARAM,
	KEPT_SAFE,
} KeptBy;

typedef struct Cand
{
	Path	   *path;
	int			pos;			/* position among the candidates */
	double		main;			/* main objective */
	double		retain;			/* retain strategy: its score, or for a
								 * diversification function the order it
								 * picks in (-1: not reached) */
	double		mean_startup;
	double		mean_total;
	KeptBy		by;
} Cand;

/* --------------------------------------------------------------- GUC */

/* An objective: a score per path, the lower the better */
static bool
is_objective(int id)
{
	switch (id)
	{
		case OBJ_WORST_PENALTY:
		case OBJ_EXPECTED_PENALTY:
		case OBJ_WORST_TOTAL:
		case OBJ_EXPECTED_TOTAL:
		case OBJ_WORST_STARTUP:
		case OBJ_EXPECTED_STARTUP:
		case OBJ_PENALTY_COUNT:
		case OBJ_POINT_TOTAL:
		case OBJ_WORST_PENALTY_STD:
		case OBJ_EXPECTED_PENALTY_STD:
		case OBJ_QUANTILE_PENALTY:
		case OBJ_QUANTILE_TOTAL:
		case OBJ_QUANTILE_STARTUP:
			return true;
	}
	return false;
}

/* A diversification function: picks a set of paths, has no score */
static bool
is_diversifier(int id)
{
	switch (id)
	{
		case DV_RANDOM:
		case DV_SIMILARITY:
		case DV_MIX:
		case DV_PENALTY_JOIN:
		case DV_COST_JOIN:
			return true;
	}
	return false;
}

bool
check_robust_objective(int *newval, void **extra, GucSource source)
{
	if (is_objective(*newval))
		return true;
	if (is_diversifier(*newval))
		GUC_check_errdetail("%d is a diversification function; it can be the retain strategy (retain_strategy_id) only.",
							*newval);
	else
		GUC_check_errdetail("Objective %d is not implemented.", *newval);
	return false;
}

bool
check_robust_strategy(int *newval, void **extra, GucSource source)
{
	if (is_objective(*newval) || is_diversifier(*newval))
		return true;
	GUC_check_errdetail("Strategy %d is not implemented.", *newval);
	return false;
}

static Loss
objective_loss(int id)
{
	switch (id)
	{
		case OBJ_PENALTY_COUNT:
			return LOSS_EXCESS;
		case OBJ_WORST_TOTAL:
		case OBJ_EXPECTED_TOTAL:
		case OBJ_QUANTILE_TOTAL:
			return LOSS_TOTAL;
		case OBJ_WORST_STARTUP:
		case OBJ_EXPECTED_STARTUP:
		case OBJ_QUANTILE_STARTUP:
			return LOSS_STARTUP;
		case OBJ_POINT_TOTAL:
			return LOSS_POINT;
	}
	return LOSS_PENALTY;
}

static Agg
objective_agg(int id)
{
	switch (id)
	{
		case OBJ_PENALTY_COUNT:
			return AGG_SUM;
		case OBJ_WORST_PENALTY:
		case OBJ_WORST_TOTAL:
		case OBJ_WORST_STARTUP:
			return AGG_MAX;
		case OBJ_WORST_PENALTY_STD:
			return AGG_MAX_STD;
		case OBJ_EXPECTED_PENALTY_STD:
			return AGG_MEAN_STD;
		case OBJ_QUANTILE_PENALTY:
		case OBJ_QUANTILE_TOTAL:
		case OBJ_QUANTILE_STARTUP:
			return AGG_QUANTILE;
	}
	return AGG_MEAN;
}

/* Does the objective or diversification function use the envelope? */
bool
robust_objective_needs_envelope(int objective)
{
	if (is_diversifier(objective))
		return objective != DV_RANDOM && objective != DV_COST_JOIN;
	if (!is_objective(objective))
		return false;
	return objective_loss(objective) == LOSS_PENALTY ||
		objective_loss(objective) == LOSS_EXCESS;
}

/* Is the main pass building joins from every path its inputs keep? */
bool
robust_select_retained(PlannerInfo *root)
{
	return enable_rows_dist && robust_select &&
		robust_join_pairing == ROBUST_PAIRING_RETAINED &&
		root->robust_pass == ROBUST_PASS_MAIN;
}

/* ---------------------------------------------------------- vectors */

static inline double
at(const RobustVec *v, double scalar, int s)
{
	return v != NULL ? robust_vec_at(v, s) : scalar;
}

/*
 * Mean of a vector, the scalar standing for a vector of N copies.  The sums
 * run over s = 0 .. N-1 in order, so the test harness computes the same
 * doubles.
 */
static double
vec_mean(const RobustVec *v, double scalar)
{
	double		sum = 0.0;

	for (int s = 0; s < error_sample_count; s++)
		sum += at(v, scalar, s);
	return sum / error_sample_count;
}

static int
cmp_double(const void *a, const void *b)
{
	double		x = *(const double *) a;
	double		y = *(const double *) b;

	return x < y ? -1 : x > y ? 1 : 0;
}

/*
 * The objective's value of the losses x[0 .. N-1] of a path.  The maximum
 * starts at 0 (as in v20; losses are not negative).  The standard deviation
 * is the population's, over the N scenarios, with the mean subtracted first.
 * The alpha-quantile is the smallest x with at least alpha N of the x not
 * above it: the ceil(alpha N)-th smallest (Quant_alpha in the paper; v20's
 * "4th worst" is alpha 0.85 with N = 20).
 *
 * Here and below, a product is never added in the same expression: the
 * compiler may fuse a * b + c into one rounding (clang does at -O0 here),
 * and the gates recompute these doubles exactly.
 */
static double
aggregate(Agg agg, double *x, int N)
{
	double		m = 0.0;
	double		sum = 0.0;
	double		mean;
	double		ss = 0.0;
	double		w;
	int			k;

	for (int s = 0; s < N; s++)
	{
		sum += x[s];
		if (x[s] > m)
			m = x[s];
	}
	mean = sum / N;
	switch (agg)
	{
		case AGG_MAX:
			return m;
		case AGG_MEAN:
			return mean;
		case AGG_SUM:
			return sum;
		case AGG_MEAN_STD:
		case AGG_MAX_STD:
			for (int s = 0; s < N; s++)
			{
				double		d = x[s] - mean;
				double		sq = d * d;

				ss += sq;
			}
			w = robust_penalty_lambda * sqrt(ss / N);
			return (agg == AGG_MEAN_STD ? mean : m) + w;
		case AGG_QUANTILE:
			w = robust_quantile_alpha * N;
			k = (int) ceil(w - 1e-9);
			k = Max(1, Min(N, k));
			qsort(x, N, sizeof(double), cmp_double);
			return x[k - 1];
	}
	return mean;
}

/*
 * The loss in scenario s of a path costing 'cost' (and 'startup') there,
 * against the envelope value 'env' (NaN: none, no penalty).
 */
static double
loss_at(Loss loss, double cost, double startup, double env)
{
	switch (loss)
	{
		case LOSS_PENALTY:
		case LOSS_EXCESS:
			if (isnan(env) || !(cost > env * (1.0 + robust_penalty_tau)))
				return 0.0;
			return loss == LOSS_PENALTY ? cost - env : 1.0;
		case LOSS_TOTAL:
		case LOSS_POINT:
			return cost;
		case LOSS_STARTUP:
			return startup;
	}
	return cost;
}

/* The objective's score of a path, against the relation's envelope */
static double
objective(int id, Path *path, const double *envelope)
{
	const RobustPathCost *rc = path->robust;
	int			N = error_sample_count;
	Loss		loss = objective_loss(id);
	double		x[ROBUST_MAX_SAMPLES];

	if (!is_objective(id))
		elog(ERROR, "RobDP objective %d is not implemented", id);
	if (loss == LOSS_POINT)
		return path->total_cost;
	for (int s = 0; s < N; s++)
		x[s] = loss_at(loss, at(rc ? rc->total : NULL, path->total_cost, s),
					   at(rc ? rc->startup : NULL, path->startup_cost, s),
					   envelope ? envelope[s] : NAN);
	return aggregate(objective_agg(id), x, N);
}

/* Equal vectors, NULL standing for N copies of the scalar */
static bool
same_vec(const RobustVec *a, double sa, const RobustVec *b, double sb)
{
	if (a == NULL && b == NULL)
		return sa == sb;
	for (int s = 0; s < error_sample_count; s++)
	{
		if (at(a, sa, s) != at(b, sb, s))
			return false;
	}
	return true;
}

/*
 * Is b the same candidate as a?  The same parameterization, sort order,
 * parallel safety and workers, rows and costs in every scenario.  The join
 * builders offer such twins when they meet the same inputs more than once
 * (retained pairing); PostgreSQL's add_path keeps one of them.
 */
static bool
same_candidate(Path *a, Path *b)
{
	const RobustPathCost *ra = a->robust;
	const RobustPathCost *rb = b->robust;

	if (a->total_cost != b->total_cost ||
		a->startup_cost != b->startup_cost ||
		a->rows != b->rows ||
		a->parallel_safe != b->parallel_safe ||
		a->parallel_workers != b->parallel_workers)
		return false;
	if (!bms_equal(PATH_REQ_OUTER(a), PATH_REQ_OUTER(b)))
		return false;
	if (compare_pathkeys(a->pathkeys, b->pathkeys) != PATHKEYS_EQUAL)
		return false;
	return same_vec(ra ? ra->rows : NULL, a->rows, rb ? rb->rows : NULL, b->rows) &&
		same_vec(ra ? ra->startup : NULL, a->startup_cost,
				 rb ? rb->startup : NULL, b->startup_cost) &&
		same_vec(ra ? ra->total : NULL, a->total_cost,
				 rb ? rb->total : NULL, b->total_cost);
}

/* ------------------------------------------------------------ twins */

/*
 * An index of a relation's pathlist or partial_pathlist in the main pass
 * by the candidates' point rows, startup and total cost, which twins share
 * (same_candidate), so that a new candidate is compared only with those
 * candidates instead of the whole list.  The index holds exactly the
 * list's elements, in order: it records the list, its length and its last
 * element, and is rebuilt from the list when any of them is not what
 * robust_select_offer() left (the list of a relation made dummy is
 * replaced; the selection replaces it with the kept paths).  Entries are
 * chained per bucket; the buckets double when they are 3/4 full.
 */
typedef struct TwinIndex
{
	const List *list;			/* the list indexed */
	int			n;				/* its length: the entries */
	const Path *last;			/* its last element */
	int			cap;			/* entries allocated */
	int			nbuckets;		/* a power of 2, or 0 */
	int		   *head;			/* per bucket: its first entry, or -1 */
	int		   *next;			/* per entry: the next of its bucket, or -1 */
	uint32	   *hash;			/* per entry */
	Path	  **path;			/* per entry: the list's element */
} TwinIndex;

typedef struct RobustTwins
{
	TwinIndex	kind[2];		/* pathlist, partial_pathlist */
} RobustTwins;

/* A double's bits, with -0 as 0: equal doubles give equal words */
static inline uint64
twin_word(double x)
{
	uint64		w;

	if (x == 0.0)
		return 0;
	memcpy(&w, &x, sizeof(w));
	return w;
}

static inline uint64
twin_mix(uint64 x)
{
	x ^= x >> 30;
	x *= UINT64CONST(0xbf58476d1ce4e5b9);
	x ^= x >> 27;
	x *= UINT64CONST(0x94d049bb133111eb);
	x ^= x >> 31;
	return x;
}

/* The hash of what same_candidate() first requires to be equal */
static uint32
twin_hash(const Path *p)
{
	uint64		h;

	h = twin_mix(twin_word(p->total_cost));
	h = twin_mix(h ^ twin_word(p->startup_cost));
	h = twin_mix(h ^ twin_word(p->rows));
	return (uint32) (h ^ (h >> 32));
}

static void
twin_insert(TwinIndex *t, MemoryContext cxt, Path *path, uint32 h)
{
	int			i;
	int			b;

	if (t->n == t->cap)
	{
		int			cap = Max(16, 2 * t->cap);

		if (t->cap == 0)
		{
			t->next = MemoryContextAlloc(cxt, sizeof(int) * cap);
			t->hash = MemoryContextAlloc(cxt, sizeof(uint32) * cap);
			t->path = MemoryContextAlloc(cxt, sizeof(Path *) * cap);
		}
		else
		{
			t->next = repalloc(t->next, sizeof(int) * cap);
			t->hash = repalloc(t->hash, sizeof(uint32) * cap);
			t->path = repalloc(t->path, sizeof(Path *) * cap);
		}
		t->cap = cap;
	}
	if (4 * (t->n + 1) > 3 * t->nbuckets)
	{
		int			nb = Max(32, 2 * t->nbuckets);

		if (t->nbuckets == 0)
			t->head = MemoryContextAlloc(cxt, sizeof(int) * nb);
		else
			t->head = repalloc(t->head, sizeof(int) * nb);
		t->nbuckets = nb;
		memset(t->head, -1, sizeof(int) * nb);
		for (int j = 0; j < t->n; j++)
		{
			b = t->hash[j] & (nb - 1);
			t->next[j] = t->head[b];
			t->head[b] = j;
		}
	}
	i = t->n++;
	t->path[i] = path;
	t->hash[i] = h;
	b = h & (t->nbuckets - 1);
	t->next[i] = t->head[b];
	t->head[b] = i;
}

static void
twin_rebuild(TwinIndex *t, MemoryContext cxt, List *list)
{
	ListCell   *lc;

	t->n = 0;
	if (t->nbuckets > 0)
		memset(t->head, -1, sizeof(int) * t->nbuckets);
	foreach(lc, list)
		twin_insert(t, cxt, (Path *) lfirst(lc), twin_hash((Path *) lfirst(lc)));
	t->list = list;
	t->last = list != NIL ? (Path *) llast(list) : NULL;
}

/*
 * robust_select_offer
 *		add_path() / add_partial_path() in the main pass of a selecting
 *		search: append the candidate to rel's pathlist (partial_pathlist),
 *		unless the list has its twin (with robust_select_dedup on).
 *		Returns whether it was appended.
 *
 * The twins are found through the relation's TwinIndex.  With
 * robust_debug_check_costs on, the list is scanned as well, and an answer
 * of the index that is not the scan's is an error.
 */
bool
robust_select_offer(RelOptInfo *rel, bool partial, Path *path)
{
	List	  **pathlist = partial ? &rel->partial_pathlist : &rel->pathlist;
	MemoryContext cxt;
	TwinIndex  *t;
	uint32		h;
	bool		twin = false;

	if (!robust_select_dedup)
	{
		*pathlist = lappend(*pathlist, path);
		return true;
	}
	cxt = GetMemoryChunkContext(rel);
	if (rel->robust_twins == NULL)
		rel->robust_twins = MemoryContextAllocZero(cxt, sizeof(RobustTwins));
	t = &rel->robust_twins->kind[partial ? 1 : 0];
	if (t->list != *pathlist || t->n != list_length(*pathlist) ||
		(t->n > 0 && t->last != (Path *) llast(*pathlist)))
		twin_rebuild(t, cxt, *pathlist);

	h = twin_hash(path);
	if (t->nbuckets > 0)
	{
		for (int i = t->head[h & (t->nbuckets - 1)]; i >= 0; i = t->next[i])
		{
			if (t->hash[i] == h && same_candidate(t->path[i], path))
			{
				twin = true;
				break;
			}
		}
	}
	if (robust_debug_check_costs)
	{
		bool		scan = false;
		ListCell   *lc;

		foreach(lc, *pathlist)
		{
			if (same_candidate((Path *) lfirst(lc), path))
			{
				scan = true;
				break;
			}
		}
		if (scan != twin)
			elog(ERROR, "RobDP: twin index of %s path list of %d paths finds %s twin, a scan of the list %s",
				 partial ? "a partial" : "a", list_length(*pathlist),
				 twin ? "a" : "no", scan ? "one" : "none");
	}
	if (twin)
		return false;

	*pathlist = lappend(*pathlist, path);
	twin_insert(t, cxt, path, h);
	t->list = *pathlist;
	t->last = path;
	return true;
}

/* ---------------------------------------------------------- precheck */

/*
 * The precheck's state for the complete or the partial candidates of a
 * join relation (optimizer/robustselect.h): the smallest main scores (and
 * retain scores) of the candidates offered so far, ascending.
 */
typedef struct PrecheckKind
{
	bool		init;
	bool		active;			/* the precheck applies */
	const double *envelope;		/* NULL if no objective in use needs it */
	int			nscored;		/* candidates of the list scored so far */
	int			kmain;
	int			nmain;
	double	   *main;
	int			kretain;		/* 0: no separate retain bound */
	int			nretain;
	double	   *retain;
	double		best_unparam;	/* complete: best main score, unparameterized */
	double		best_safe;		/* ... and parallel-safe */
	int			dropped;
} PrecheckKind;

typedef struct RobustPrecheck
{
	PrecheckKind kind[2];		/* complete, partial */
} RobustPrecheck;

/* Relative margin of the lower bounds, for the rounding of the cost sums */
#define PRECHECK_MARGIN (1.0 - 1e-9)

/* Is the objective monotone in each scenario's total and startup cost? */
static bool
precheck_supports(int id)
{
	return is_objective(id) &&
		objective_agg(id) != AGG_MEAN_STD &&
		objective_agg(id) != AGG_MAX_STD;
}

/* Insert x into the ascending k smallest a[0 .. *n-1] */
static void
smallest_insert(double *a, int *n, int k, double x)
{
	int			i;

	if (*n == k)
	{
		if (!(x < a[k - 1]))
			return;
		(*n)--;
	}
	for (i = *n; i > 0 && a[i - 1] > x; i--)
		a[i] = a[i - 1];
	a[i] = x;
	(*n)++;
}

static void
precheck_init(PlannerInfo *root, RelOptInfo *rel, bool partial,
			  PrecheckKind *k)
{
	MemoryContext cxt = GetMemoryChunkContext(rel);
	bool		retain = retain_path_limit > 0;
	bool		uses_envelope;
	const double *envelope;

	memset(k, 0, sizeof(PrecheckKind));
	k->init = true;
	k->best_unparam = k->best_safe = INFINITY;
	if (!precheck_supports(main_objective_id) ||
		(retain && !precheck_supports(retain_strategy_id)))
		return;

	/* The envelope as the selection's, unless it would fill it */
	uses_envelope = robust_objective_needs_envelope(main_objective_id) ||
		(retain && robust_objective_needs_envelope(retain_strategy_id));
	if (uses_envelope)
	{
		envelope = robust_search_envelope(root, rel, partial);
		if (envelope == NULL)
			return;
		for (int s = 0; s < error_sample_count; s++)
		{
			if (isnan(envelope[s]))
				return;
		}
		k->envelope = envelope;
	}

	/* As many as any selection of the relation keeps (top or not) */
	if (retain && retain_strategy_id == main_objective_id)
		k->kmain = Max(add_path_limit + retain_path_limit,
					   robust_final_path_limit);
	else
	{
		k->kmain = Max(add_path_limit, robust_final_path_limit);
		if (retain)
			k->kretain = add_path_limit + retain_path_limit;
	}
	k->main = MemoryContextAlloc(cxt, sizeof(double) * k->kmain);
	if (k->kretain > 0)
		k->retain = MemoryContextAlloc(cxt, sizeof(double) * k->kretain);
	k->active = true;
}

/* Score the candidates offered since the last call */
static void
precheck_catch_up(PrecheckKind *k, List *list)
{
	for (int i = k->nscored; i < list_length(list); i++)
	{
		Path	   *path = (Path *) list_nth(list, i);
		double		m = objective(main_objective_id, path, k->envelope);

		smallest_insert(k->main, &k->nmain, k->kmain, m);
		if (k->kretain > 0)
			smallest_insert(k->retain, &k->nretain, k->kretain,
							objective(retain_strategy_id, path, k->envelope));
		if (path->param_info == NULL)
		{
			k->best_unparam = Min(k->best_unparam, m);
			if (path->parallel_safe)
				k->best_safe = Min(k->best_safe, m);
		}
	}
	k->nscored = list_length(list);
}

/* An input path's total or startup cost in scenario s; 0 if not costed yet */
static inline double
input_cost(Path *path, bool total, int s)
{
	const RobustPathCost *rc = path->robust;

	if (rc == NULL)
		return 0.0;
	return total ? at(rc->total, path->total_cost, s) :
		at(rc->startup, path->startup_cost, s);
}

/* An objective's score of the cost vectors total[], startup[] */
static double
objective_of(int id, const double *total, const double *startup,
			 double point, const double *envelope)
{
	Loss		loss = objective_loss(id);
	double		x[ROBUST_MAX_SAMPLES];

	if (loss == LOSS_POINT)
		return point;
	for (int s = 0; s < error_sample_count; s++)
		x[s] = loss_at(loss, total[s], startup[s],
					   envelope ? envelope[s] : NAN);
	return aggregate(objective_agg(id), x, error_sample_count);
}

/*
 * robust_precheck_join
 *		May the join builders build and cost this join path candidate of
 *		joinrel, by 'method' from outer_path and inner_path (a merge join:
 *		'outer_sorted' / 'inner_sorted' if it sorts that side)?  False when
 *		the selection could never keep it (optimizer/robustselect.h).
 *		'parameterized': the candidate will be.
 */
bool
robust_precheck_join(PlannerInfo *root, RelOptInfo *joinrel, bool partial,
					 RobustJoinBound method, Path *outer_path,
					 Path *inner_path, bool outer_sorted, bool inner_sorted,
					 bool parameterized)
{
	PrecheckKind *k;
	List	   *list = partial ? joinrel->partial_pathlist : joinrel->pathlist;
	double		total[ROBUST_MAX_SAMPLES];
	double		startup[ROBUST_MAX_SAMPLES];
	double		point;
	double		bound;

	if (!enable_rows_dist || !robust_select_precheck || !robust_cost_collect() ||
		root->robust_search == NULL || IS_OTHER_REL(joinrel))
		return true;
	if (joinrel->robust_precheck == NULL)
		joinrel->robust_precheck =
			MemoryContextAllocZero(GetMemoryChunkContext(joinrel),
								   sizeof(RobustPrecheck));
	k = &joinrel->robust_precheck->kind[partial ? 1 : 0];
	if (k->init && list_length(list) < k->nscored)
		k->init = false;		/* the list was replaced (a dummy relation) */
	if (!k->init)
		precheck_init(root, joinrel, partial, k);
	if (!k->active)
		return true;
	precheck_catch_up(k, list);
	if (k->nmain < k->kmain || k->nretain < k->kretain)
		return true;

	/* Lower bounds of the candidate's costs in each scenario */
	for (int s = 0; s < error_sample_count; s++)
	{
		double		o_total = input_cost(outer_path, true, s);
		double		o_startup = input_cost(outer_path, false, s);
		double		i_total = input_cost(inner_path, true, s);
		double		i_startup = input_cost(inner_path, false, s);
		double		t,
					u;

		switch (method)
		{
			case ROBUST_BOUND_NESTLOOP:
				t = o_total + i_startup;
				u = o_startup + i_startup;
				break;
			case ROBUST_BOUND_HASHJOIN:
				t = o_total + i_total;
				u = o_startup + i_total;
				break;
			default:
				t = (outer_sorted ? o_total : o_startup) +
					(inner_sorted ? i_total : i_startup);
				u = t;
				break;
		}
		total[s] = t * PRECHECK_MARGIN;
		startup[s] = u * PRECHECK_MARGIN;
	}
	switch (method)
	{
		case ROBUST_BOUND_NESTLOOP:
			point = outer_path->total_cost + inner_path->startup_cost;
			break;
		case ROBUST_BOUND_HASHJOIN:
			point = outer_path->total_cost + inner_path->total_cost;
			break;
		default:
			point = (outer_sorted ? outer_path->total_cost :
					 outer_path->startup_cost) +
				(inner_sorted ? inner_path->total_cost :
				 inner_path->startup_cost);
			break;
	}
	point *= PRECHECK_MARGIN;

	/* K candidates better by the main objective */
	bound = objective_of(main_objective_id, total, startup, point, k->envelope);
	if (!(k->main[k->kmain - 1] < bound))
		return true;
	/* add_path_limit + retain_path_limit by the retain strategy */
	if (k->kretain > 0 &&
		!(k->retain[k->kretain - 1] <
		  objective_of(retain_strategy_id, total, startup, point, k->envelope)))
		return true;
	/* not the best unparameterized (parallel-safe) candidate */
	if (!partial && !parameterized)
	{
		if (!(k->best_unparam < bound))
			return true;
		if (joinrel->consider_parallel && outer_path->parallel_safe &&
			inner_path->parallel_safe && !(k->best_safe < bound))
			return true;
	}
	k->dropped++;
	return false;
}

/* How many candidates the precheck dropped; forget its state */
static int
precheck_done(RelOptInfo *rel, bool partial)
{
	PrecheckKind *k;
	int			dropped;

	if (rel->robust_precheck == NULL)
		return 0;
	k = &rel->robust_precheck->kind[partial ? 1 : 0];
	dropped = k->dropped;
	if (k->main)
		pfree(k->main);
	if (k->retain)
		pfree(k->retain);
	memset(k, 0, sizeof(PrecheckKind));
	return dropped;
}

/* --------------------------------------------------------- selection */

/*
 * Orders: by the key, then the expected total cost, then as PostgreSQL's
 * compare_path_costs (total, then startup cost), then the position.  So
 * set_cheapest, which keeps the first of equals, takes the first path kept.
 */
static int
cmp_key(double a, double b)
{
	return a < b ? -1 : a > b ? 1 : 0;
}

static int
cmp_rest(const Cand *x, const Cand *y)
{
	int			c;

	if ((c = cmp_key(x->mean_total, y->mean_total)) != 0)
		return c;
	if ((c = cmp_key(x->path->total_cost, y->path->total_cost)) != 0)
		return c;
	if ((c = cmp_key(x->path->startup_cost, y->path->startup_cost)) != 0)
		return c;
	return cmp_key(x->pos, y->pos);
}

static int
cmp_main(const void *a, const void *b)
{
	const Cand *x = *(Cand *const *) a;
	const Cand *y = *(Cand *const *) b;
	int			c;

	if ((c = cmp_key(x->main, y->main)) != 0)
		return c;
	return cmp_rest(x, y);
}

static int
cmp_retain(const void *a, const void *b)
{
	const Cand *x = *(Cand *const *) a;
	const Cand *y = *(Cand *const *) b;
	int			c;

	if ((c = cmp_key(x->retain, y->retain)) != 0)
		return c;
	return cmp_rest(x, y);
}

/* ------------------------------------------ diversification functions */

/*
 * A diversification function (or an objective, in Mix) picks paths to
 * retain from the candidates the main objective did not keep, rest[0 ..
 * n-1], one at a time: dv_next() gives its next pick not taken yet (by it or
 * another function of Mix), or -1.  Objectives pick in the order of their
 * score, then cmp_rest.
 */
typedef struct Dv
{
	int			id;
	Cand	  **rest;
	int			n;
	const bool *taken;
	int		   *order;			/* objectives, random, join: all, in order */
	int			norder;
	int			next;
	/* similarity */
	const double *envelope;
	double	   *cost;			/* [i * N + s]: total(s) of rest[i] */
	bool	   *eligible;		/* not pruned */
	bool	   *center;			/* its own picks, taken or not */
	double	   *min_dist;		/* to the nearest center */
	int			ncenters;
	int		   *rank;			/* position in the cmp_rest order */
} Dv;

typedef struct Keyed
{
	double		key;
	Cand	   *cand;
	int			i;
} Keyed;

static int
cmp_keyed(const void *a, const void *b)
{
	const Keyed *x = (const Keyed *) a;
	const Keyed *y = (const Keyed *) b;
	int			c;

	if ((c = cmp_key(x->key, y->key)) != 0)
		return c;
	return cmp_rest(x->cand, y->cand);
}

/* rest[] sorted by key[] (then cmp_rest), into order[] */
static void
sort_by_key(Cand **rest, int n, const double *key, int *order)
{
	Keyed	   *k = palloc(sizeof(Keyed) * Max(n, 1));

	for (int i = 0; i < n; i++)
	{
		k[i].key = key[i];
		k[i].cand = rest[i];
		k[i].i = i;
	}
	qsort(k, n, sizeof(Keyed), cmp_keyed);
	for (int i = 0; i < n; i++)
		order[i] = k[i].i;
	pfree(k);
}

/*
 * Rand: uniform draws, from a stream seeded by error_sample_seed and the
 * relation (v20 drew from one generator for the whole backend, so a plan
 * depended on what was planned before).  The draws go into retain.
 */
static void
dv_random_order(Dv *dv, RelOptInfo *rel, bool partial)
{
	pg_prng_state prng;
	double	   *key = palloc(sizeof(double) * Max(dv->n, 1));
	uint64		seed;

	seed = ((uint64) (uint32) error_sample_seed << 32) ^
		(uint64) bms_hash_value(rel->relids) ^
		(partial ? UINT64CONST(0x9E3779B97F4A7C15) : 0);
	pg_prng_seed(&prng, seed);
	for (int i = 0; i < dv->n; i++)
	{
		key[i] = pg_prng_double(&prng);
		dv->rest[i]->retain = key[i];
	}
	sort_by_key(dv->rest, dv->n, key, dv->order);
	dv->norder = dv->n;
	pfree(key);
}

/*
 * E[Penalty]-Join, E[Cost]-Join: the candidates grouped by their last
 * operator (pathtype: the join method, or the scan), the groups in the order
 * of their best; then round by round, each group's next best.
 */
static void
dv_join_order(Dv *dv, const double *envelope)
{
	int			obj = dv->id == DV_PENALTY_JOIN ? OBJ_EXPECTED_PENALTY :
		OBJ_EXPECTED_TOTAL;
	double	   *key = palloc(sizeof(double) * Max(dv->n, 1));
	int		   *sorted = palloc(sizeof(int) * Max(dv->n, 1));
	int		   *group = palloc(sizeof(int) * Max(dv->n, 1));
	NodeTag    *gkey = palloc(sizeof(NodeTag) * Max(dv->n, 1));
	int			ngroups = 0;
	int			out = 0;

	for (int i = 0; i < dv->n; i++)
		key[i] = objective(obj, dv->rest[i]->path, envelope);
	sort_by_key(dv->rest, dv->n, key, sorted);
	for (int j = 0; j < dv->n; j++)
	{
		NodeTag		t = dv->rest[sorted[j]]->path->pathtype;
		int			g;

		for (g = 0; g < ngroups && gkey[g] != t; g++)
			;
		if (g == ngroups)
			gkey[ngroups++] = t;
		group[j] = g;
	}
	for (int round = 0; out < dv->n; round++)
	{
		for (int g = 0; g < ngroups; g++)
		{
			int			seen = 0;

			for (int j = 0; j < dv->n; j++)
			{
				if (group[j] != g)
					continue;
				if (seen++ == round)
				{
					dv->order[out++] = sorted[j];
					break;
				}
			}
		}
	}
	dv->norder = out;
	pfree(key);
	pfree(sorted);
	pfree(group);
	pfree(gkey);
}

/*
 * The Jensen-Shannon distance of two cost vectors, each normalized to sum
 * to 1 (as v20 and PARQO's k_center_greedy): sqrt(KL(p || m) / 2 + KL(q ||
 * m) / 2), m = (p + q) / 2, with 1e-10 added to each normalized entry.
 */
static double
kl_divergence(const double *p, const double *q, int N)
{
	double		sp = 0.0;
	double		sq = 0.0;
	double		kl = 0.0;

	for (int s = 0; s < N; s++)
	{
		sp += p[s];
		sq += q[s];
	}
	if (sp <= 0.0 || sq <= 0.0)
		return 0.0;
	for (int s = 0; s < N; s++)
	{
		double		pi = p[s] / sp + 1e-10;
		double		qi = q[s] / sq + 1e-10;
		double		term = pi * log(pi / qi);

		kl += term;
	}
	return kl;
}

static double
js_distance(const double *p, const double *q, int N)
{
	double		m[ROBUST_MAX_SAMPLES];
	double		a;
	double		b;
	double		d;

	for (int s = 0; s < N; s++)
		m[s] = 0.5 * (p[s] + q[s]);
	a = 0.5 * kl_divergence(p, m, N);
	b = 0.5 * kl_divergence(q, m, N);
	d = a + b;
	if (d < 0.0)
		d = 0.0;
	return sqrt(d);
}

/*
 * Similarity (the paper's Section 4.2.2): farthest-first k-center over the
 * candidates' total cost vectors, by the Jensen-Shannon distance, among the
 * candidates that do not exceed theta = 2 max_s C*(s) in every scenario
 * (all of them if there is no envelope; the one of least expected cost if
 * none is left), starting from the candidate of least expected cost.  Ties
 * go to the earlier in the cmp_rest order.  v20 started from the least
 * point cost, took as many centers as retain_path_limit whatever its
 * budget, and could pick a center twice.
 */
static void
dv_similarity_setup(Dv *dv, const double *envelope)
{
	int			N = error_sample_count;
	double		theta = NAN;
	int		   *order = palloc(sizeof(int) * Max(dv->n, 1));
	double	   *key = palloc(sizeof(double) * Max(dv->n, 1));
	bool		any = false;

	dv->envelope = envelope;
	dv->cost = palloc(sizeof(double) * Max(dv->n, 1) * N);
	dv->eligible = palloc(sizeof(bool) * Max(dv->n, 1));
	dv->center = palloc0(sizeof(bool) * Max(dv->n, 1));
	dv->min_dist = palloc(sizeof(double) * Max(dv->n, 1));
	dv->rank = palloc(sizeof(int) * Max(dv->n, 1));
	if (envelope != NULL)
	{
		for (int s = 0; s < N; s++)
		{
			if (!isnan(envelope[s]) && (isnan(theta) || envelope[s] > theta))
				theta = envelope[s];
		}
		if (!isnan(theta))
			theta *= 2.0;
	}
	for (int i = 0; i < dv->n; i++)
	{
		Path	   *path = dv->rest[i]->path;
		const RobustVec *tv = path->robust ? path->robust->total : NULL;
		bool		below = isnan(theta);

		for (int s = 0; s < N; s++)
		{
			dv->cost[i * N + s] = at(tv, path->total_cost, s);
			if (dv->cost[i * N + s] <= theta)
				below = true;
		}
		dv->eligible[i] = below;
		any |= below;
		dv->min_dist[i] = DBL_MAX;
		key[i] = 0.0;
	}
	sort_by_key(dv->rest, dv->n, key, order);	/* the cmp_rest order */
	for (int j = 0; j < dv->n; j++)
		dv->rank[order[j]] = j;
	if (!any && dv->n > 0)
		dv->eligible[order[0]] = true;
	pfree(order);
	pfree(key);
}

/* The next center, -1 when every eligible candidate is one */
static int
dv_similarity_center(Dv *dv)
{
	int			N = error_sample_count;
	int			best = -1;

	for (int i = 0; i < dv->n; i++)
	{
		if (!dv->eligible[i] || dv->center[i])
			continue;
		if (best < 0 ||
			(dv->ncenters > 0 && dv->min_dist[i] > dv->min_dist[best]) ||
			((dv->ncenters == 0 || dv->min_dist[i] == dv->min_dist[best]) &&
			 dv->rank[i] < dv->rank[best]))
			best = i;
	}
	if (best < 0)
		return -1;
	dv->center[best] = true;
	dv->ncenters++;
	for (int i = 0; i < dv->n; i++)
	{
		double		d;

		if (!dv->eligible[i] || dv->center[i])
			continue;
		d = js_distance(&dv->cost[i * N], &dv->cost[best * N], N);
		if (d < dv->min_dist[i])
			dv->min_dist[i] = d;
	}
	return best;
}

static void
dv_init(Dv *dv, int id, Cand **rest, int n, const bool *taken,
		RelOptInfo *rel, bool partial, const double *envelope)
{
	memset(dv, 0, sizeof(Dv));
	dv->id = id;
	dv->rest = rest;
	dv->n = n;
	dv->taken = taken;
	dv->order = palloc(sizeof(int) * Max(n, 1));
	if (is_objective(id))
	{
		double	   *key = palloc(sizeof(double) * Max(n, 1));

		for (int i = 0; i < n; i++)
			key[i] = objective(id, rest[i]->path, envelope);
		sort_by_key(rest, n, key, dv->order);
		dv->norder = n;
		pfree(key);
	}
	else if (id == DV_RANDOM)
		dv_random_order(dv, rel, partial);
	else if (id == DV_PENALTY_JOIN || id == DV_COST_JOIN)
		dv_join_order(dv, envelope);
	else if (id == DV_SIMILARITY)
		dv_similarity_setup(dv, envelope);
	else
		elog(ERROR, "RobDP diversification function %d is not implemented", id);
}

static int
dv_next(Dv *dv)
{
	if (dv->id == DV_SIMILARITY)
	{
		int			i;

		while ((i = dv_similarity_center(dv)) >= 0)
		{
			if (!dv->taken[i])
				return i;
		}
		return -1;
	}
	while (dv->next < dv->norder)
	{
		int			i = dv->order[dv->next++];

		if (!dv->taken[i])
			return i;
	}
	return -1;
}

/*
 * Mix: the paper's ensemble of seven, the budget shared evenly by picking
 * from each in turn.  v20's "retention set" (14) had worst penalty in place
 * of the quantile, no E[Cost]-Join, and its coverage member took the first
 * candidate in list order.
 */
static const int mix_members[] = {
	OBJ_EXPECTED_TOTAL, OBJ_EXPECTED_PENALTY, OBJ_EXPECTED_PENALTY_STD,
	OBJ_QUANTILE_PENALTY, OBJ_PENALTY_COUNT, DV_SIMILARITY, DV_COST_JOIN,
};

/*
 * Retain up to k of rest[0 .. n-1] by the diversification function id.
 * The picks get KEPT_RETAIN and, but for Rand (its draws), their order of
 * picking in retain (the others -1).
 */
static void
diversify(int id, RelOptInfo *rel, bool partial, Cand **rest, int n, int k,
		  const double *envelope)
{
	bool	   *taken = palloc0(sizeof(bool) * Max(n, 1));
	int			nmembers = id == DV_MIX ? lengthof(mix_members) : 1;
	Dv		   *dv = palloc(sizeof(Dv) * nmembers);
	int			picked = 0;

	for (int i = 0; i < n; i++)
	{
		if (id != DV_RANDOM)
			rest[i]->retain = -1.0;
	}
	for (int m = 0; m < nmembers; m++)
		dv_init(&dv[m], id == DV_MIX ? mix_members[m] : id, rest, n, taken,
				rel, partial, envelope);
	while (picked < k)
	{
		bool		progress = false;

		for (int m = 0; m < nmembers && picked < k; m++)
		{
			int			i = dv_next(&dv[m]);

			if (i < 0)
				continue;
			taken[i] = true;
			rest[i]->by = KEPT_RETAIN;
			if (id != DV_RANDOM)
				rest[i]->retain = picked;
			picked++;
			progress = true;
		}
		if (!progress)
			break;
	}
	pfree(taken);
	pfree(dv);
}

static const char *
kept_by_name(KeptBy by)
{
	switch (by)
	{
		case KEPT_MAIN:
			return "main";
		case KEPT_RETAIN:
			return "retain";
		case KEPT_UNPARAM:
			return "unparam";
		case KEPT_SAFE:
			return "safe";
		default:
			return "no";
	}
}

static void
dump_ids_scores(RobustDump *d, const char *key, Cand *c, int n, bool ids,
				bool retain)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	for (int i = 0; i < n; i++)
	{
		if (i > 0)
			appendStringInfoChar(&buf, ',');
		if (ids)
			appendStringInfo(&buf, "%d",
							 c[i].path->robust ? c[i].path->robust->id : -1);
		else
			robust_json_number(&buf, retain ? c[i].retain : c[i].main);
	}
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(d, key, buf.data);
	pfree(buf.data);
}

static void
dump_keep(PlannerInfo *root, RelOptInfo *rel, bool partial, int limit,
		  int retain, bool had_envelope, int filled, int prechecked, Cand *c,
		  int n, Cand **kept, int nkept)
{
	RobustDump	d;
	StringInfoData buf;

	robust_dump_begin(&d, "keep");
	robust_dump_str(&d, "pass", robust_pass_name(root->robust_pass));
	robust_dump_relids(&d, "rels", root, rel->relids);
	robust_dump_bool(&d, "partial", partial);
	robust_dump_int(&d, "limit", limit);
	robust_dump_int(&d, "retain", retain);
	robust_dump_int(&d, "objective", main_objective_id);
	robust_dump_int(&d, "strategy", retain_strategy_id);
	robust_dump_bool(&d, "envelope", had_envelope);
	robust_dump_int(&d, "filled", filled);
	robust_dump_int(&d, "prechecked", prechecked);
	dump_ids_scores(&d, "cands", c, n, true, false);
	dump_ids_scores(&d, "main", c, n, false, false);
	if (retain > 0)
		dump_ids_scores(&d, "retain_score", c, n, false, true);
	initStringInfo(&buf);
	if (retain > 0 && (retain_strategy_id == DV_PENALTY_JOIN ||
					   retain_strategy_id == DV_COST_JOIN ||
					   retain_strategy_id == DV_MIX))
	{
		/* the last operators the join functions group by */
		appendStringInfoChar(&buf, '[');
		for (int i = 0; i < n; i++)
			appendStringInfo(&buf, "%s%d", i > 0 ? "," : "",
							 (int) c[i].path->pathtype);
		appendStringInfoChar(&buf, ']');
		robust_dump_raw(&d, "ops", buf.data);
		resetStringInfo(&buf);
	}
	appendStringInfoChar(&buf, '[');
	for (int i = 0; i < nkept; i++)
	{
		if (i > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfo(&buf, "%d",
						 kept[i]->path->robust ? kept[i]->path->robust->id : -1);
	}
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(&d, "kept", buf.data);
	resetStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	for (int i = 0; i < nkept; i++)
	{
		if (i > 0)
			appendStringInfoChar(&buf, ',');
		escape_json(&buf, kept_by_name(kept[i]->by));
	}
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(&d, "by", buf.data);
	pfree(buf.data);
	robust_dump_end(&d);
}

static double *own_envelope(List *paths, bool complete);

/*
 * The envelope a selection compares to: the relation's, with every scenario
 * it lacks taken from the candidates themselves (the cheapest of them in
 * that scenario, of the unparameterized ones for complete paths if there are
 * any, as own_envelope).  A scenario lacks its value when no path of the
 * relation was left in that envelope round: add_partial_path_precheck drops
 * every partial path that costs more than the cheapest complete one, and the
 * main pass, without the precheck, still has candidates.  The relation lacks
 * an envelope when the envelope rounds never built it.  Without an objective
 * that compares to the envelope there is none to fill.  *filled is the
 * number of scenarios taken from the candidates.
 */
static const double *
selection_envelope(List *list, bool partial, int retain,
				   const double *envelope, int *filled)
{
	double	   *own;
	double	   *env;

	*filled = 0;
	if (envelope == NULL &&
		!robust_objective_needs_envelope(main_objective_id) &&
		!(retain > 0 && robust_objective_needs_envelope(retain_strategy_id)))
		return NULL;
	if (envelope != NULL)
	{
		bool		lacks = false;

		for (int s = 0; s < error_sample_count; s++)
			lacks |= isnan(envelope[s]);
		if (!lacks)
			return envelope;
	}
	own = own_envelope(list, !partial);
	if (own == NULL)
		return envelope;
	env = palloc(sizeof(double) * error_sample_count);
	for (int s = 0; s < error_sample_count; s++)
	{
		if (envelope != NULL && !isnan(envelope[s]))
			env[s] = envelope[s];
		else
		{
			env[s] = own[s];
			(*filled)++;
		}
	}
	pfree(own);
	return env;
}

/*
 * robust_select_paths
 *		Keep the paths of rel's pathlist (or partial_pathlist) the objectives
 *		select, scored, in the order of the main objective: 'limit' by the
 *		main objective, then 'retain' by the retain strategy, then (pathlist
 *		only) the best unparameterized candidate if none was kept, and the
 *		best parallel-safe unparameterized one if none was kept.  A limit
 *		of at least the number of candidates keeps them all: the relation is
 *		only scored.  'envelope' is the relation's, for its complete or its
 *		partial paths, or NULL; the scenarios it lacks are taken from the
 *		candidates (selection_envelope).
 */
void
robust_select_paths(PlannerInfo *root, RelOptInfo *rel, bool partial,
					int limit, int retain, const double *envelope)
{
	List	   *list = partial ? rel->partial_pathlist : rel->pathlist;
	int			n = list_length(list);
	Cand	   *c;
	Cand	  **order;
	Cand	  **kept;
	int			nkept = 0;
	List	   *result = NIL;
	ListCell   *lc;
	bool		had_envelope = envelope != NULL;
	int			filled;
	int			prechecked = precheck_done(rel, partial);

	if (n == 0)
		return;
	envelope = selection_envelope(list, partial, retain, envelope, &filled);
	c = palloc(sizeof(Cand) * n);
	order = palloc(sizeof(Cand *) * n);
	kept = palloc(sizeof(Cand *) * n);
	foreach(lc, list)
	{
		Path	   *path = (Path *) lfirst(lc);
		const RobustPathCost *rc = path->robust;
		int			i = foreach_current_index(lc);

		c[i].path = path;
		c[i].pos = i;
		c[i].main = objective(main_objective_id, path, envelope);
		c[i].retain = retain > 0 && is_objective(retain_strategy_id) ?
			objective(retain_strategy_id, path, envelope) : 0.0;
		c[i].mean_startup = vec_mean(rc ? rc->startup : NULL, path->startup_cost);
		c[i].mean_total = vec_mean(rc ? rc->total : NULL, path->total_cost);
		c[i].by = KEPT_NOT;
		order[i] = &c[i];
	}

	/* The main objective's best */
	qsort(order, n, sizeof(Cand *), cmp_main);
	for (int i = 0; i < n && i < limit; i++)
		order[i]->by = KEPT_MAIN;

	/* The retain strategy's best of the others */
	if (retain > 0 && limit < n)
	{
		int			nrest = 0;
		Cand	  **rest = palloc(sizeof(Cand *) * n);

		for (int i = 0; i < n; i++)
		{
			if (c[i].by == KEPT_NOT)
				rest[nrest++] = &c[i];
		}
		if (is_objective(retain_strategy_id))
		{
			qsort(rest, nrest, sizeof(Cand *), cmp_retain);
			for (int i = 0; i < nrest && i < retain; i++)
				rest[i]->by = KEPT_RETAIN;
		}
		else
			diversify(retain_strategy_id, rel, partial, rest, nrest, retain,
					  envelope);
		pfree(rest);
	}

	/* A complete plan: the best unparameterized path if none is kept */
	if (!partial)
	{
		bool		have = false;

		for (int i = 0; i < n; i++)
		{
			if (c[i].by != KEPT_NOT && c[i].path->param_info == NULL)
				have = true;
		}
		for (int i = 0; i < n && !have; i++)
		{
			if (order[i]->path->param_info == NULL)
			{
				order[i]->by = KEPT_UNPARAM;
				have = true;
			}
		}

		/*
		 * And the best parallel-safe unparameterized path if none is kept:
		 * the join builders take a partial join's inner path from the
		 * pathlist's parallel-safe unparameterized paths
		 * (get_cheapest_parallel_safe_total_inner), and a Gather path, which
		 * is never parallel-safe, may win here.  add_path never lets a path
		 * that is not parallel-safe dominate one that is.
		 */
		have = false;
		for (int i = 0; i < n; i++)
		{
			if (c[i].by != KEPT_NOT && c[i].path->param_info == NULL &&
				c[i].path->parallel_safe)
				have = true;
		}
		for (int i = 0; i < n && !have; i++)
		{
			if (order[i]->path->param_info == NULL &&
				order[i]->path->parallel_safe)
			{
				order[i]->by = KEPT_SAFE;
				have = true;
			}
		}
	}

	/* Kept, in the main objective's order, with their scores */
	for (int i = 0; i < n; i++)
	{
		Cand	   *k = order[i];
		RobustPathCost *rc = k->path->robust;

		if (rc != NULL)
		{
			rc->scored = true;
			rc->score = k->main;
			rc->mean_startup = k->mean_startup;
			rc->mean_total = k->mean_total;
		}
		if (k->by == KEPT_NOT)
			continue;
		kept[nkept++] = k;
		result = lappend(result, k->path);
	}
	if (robust_dumping(root))
		dump_keep(root, rel, partial, limit, retain, had_envelope, filled,
				  prechecked, c, n, kept, nkept);
	if (filled > 0)
		pfree((double *) envelope);

	if (partial)
		rel->partial_pathlist = result;
	else
		rel->pathlist = result;
	pfree(c);
	pfree(order);
	pfree(kept);
}

/*
 * robust_select_compare
 *		compare_path_costs() for two paths the selection scored: by the
 *		score (TOTAL_COST) or the expected startup cost (STARTUP_COST), then
 *		the expected total cost.  0 if those are equal, for the caller to
 *		compare the scalar fields.
 */
int
robust_select_compare(Path *path1, Path *path2, CostSelector criterion)
{
	const RobustPathCost *r1 = path1->robust;
	const RobustPathCost *r2 = path2->robust;
	int			c;

	if (criterion == STARTUP_COST)
		c = cmp_key(r1->mean_startup, r2->mean_startup);
	else
		c = cmp_key(r1->score, r2->score);
	if (c != 0)
		return c;
	return cmp_key(r1->mean_total, r2->mean_total);
}

/* -------------------------------------------- above the join search */

/*
 * The cheapest total cost of paths in each scenario: of the unparameterized
 * ones if there are any ('complete'), else of all; NULL if there are none.
 */
static double *
own_envelope(List *paths, bool complete)
{
	double	   *env;
	bool		unparam = false;
	bool		any = false;
	ListCell   *lc;

	if (complete)
	{
		foreach(lc, paths)
			unparam |= ((Path *) lfirst(lc))->param_info == NULL;
	}
	env = palloc(sizeof(double) * error_sample_count);
	for (int s = 0; s < error_sample_count; s++)
		env[s] = NAN;
	foreach(lc, paths)
	{
		Path	   *path = (Path *) lfirst(lc);
		const RobustVec *tv = path->robust ? path->robust->total : NULL;

		if (unparam && path->param_info != NULL)
			continue;
		any = true;
		for (int s = 0; s < error_sample_count; s++)
		{
			double		c = at(tv, path->total_cost, s);

			if (isnan(env[s]) || c < env[s])
				env[s] = c;
		}
	}
	if (!any)
	{
		pfree(env);
		return NULL;
	}
	return env;
}

/*
 * robust_select_score_rel
 *		The scan/join relation once its final target is applied (planner.c's
 *		apply_scanjoin_target_to_paths): its paths are projections of those
 *		the selection kept, or of the paths of a relation never selected
 *		(the only relation of a query without joins), and the top Gather
 *		paths were added.  Score them all by the main objective, against the
 *		cheapest of them in each scenario, without pruning, so that the
 *		upper planning starts from the best of them by the objective.
 */
void
robust_select_score_rel(PlannerInfo *root, RelOptInfo *rel)
{
	double	   *env;
	ListCell   *lc;

	if (!enable_rows_dist || !robust_select ||
		root->robust_pass != ROBUST_PASS_UPPER)
		return;

	/* every path with its vectors (projections replaced paths in place) */
	foreach(lc, rel->pathlist)
	{
		if (((Path *) lfirst(lc))->robust == NULL)
			robust_cost_add_path((Path *) lfirst(lc), false);
	}
	foreach(lc, rel->partial_pathlist)
	{
		if (((Path *) lfirst(lc))->robust == NULL)
			robust_cost_add_path((Path *) lfirst(lc), true);
	}

	env = own_envelope(rel->partial_pathlist, false);
	robust_select_paths(root, rel, true, list_length(rel->partial_pathlist), 0,
						env);
	env = own_envelope(rel->pathlist, true);
	robust_select_paths(root, rel, false, list_length(rel->pathlist), 0, env);
}

/*
 * The cost of fetching the fraction of a path's rows in scenario s, as
 * get_cheapest_fractional_path and compare_fractional_path_costs compute
 * it: an absolute number of rows (>= 1) is a fraction of the path's rows in
 * s, and a fraction outside (0, 1) means the total cost.
 */
static double
fractional_at(Path *path, double tuple_fraction, int s)
{
	const RobustPathCost *rc = path->robust;
	double		total = at(rc ? rc->total : NULL, path->total_cost, s);
	double		startup = at(rc ? rc->startup : NULL, path->startup_cost, s);
	double		rows = at(rc ? rc->rows : NULL, path->rows, s);
	double		f = tuple_fraction;

	if (f >= 1.0 && rows > 0)
		f = f / rows;
	if (f <= 0.0 || f >= 1.0)
		return total;
	f *= total - startup;
	return startup + f;
}

/* likewise at the point (s = -1) */
static double
fractional_point(Path *path, double tuple_fraction)
{
	double		f = tuple_fraction;

	if (f >= 1.0 && path->rows > 0)
		f = f / path->rows;
	if (f <= 0.0 || f >= 1.0)
		return path->total_cost;
	f *= path->total_cost - path->startup_cost;
	return path->startup_cost + f;
}

typedef struct FinalCand
{
	Path	   *path;
	int			pos;
	double		score;
	double		mean_frac;
	double		point_frac;
} FinalCand;

static int
cmp_final(const void *a, const void *b)
{
	const FinalCand *x = (const FinalCand *) a;
	const FinalCand *y = (const FinalCand *) b;
	int			c;

	if ((c = cmp_key(x->score, y->score)) != 0)
		return c;
	if ((c = cmp_key(x->mean_frac, y->mean_frac)) != 0)
		return c;
	if ((c = cmp_key(x->point_frac, y->point_frac)) != 0)
		return c;
	if ((c = cmp_key(x->path->startup_cost, y->path->startup_cost)) != 0)
		return c;
	return cmp_key(x->pos, y->pos);
}

static bool
has_vectors(Path *path)
{
	const RobustPathCost *rc = path->robust;

	return rc != NULL && (rc->rows != NULL || rc->startup != NULL ||
						  rc->total != NULL);
}

/*
 * robust_select_final
 *		get_cheapest_fractional_path() in robust planning: the path of a
 *		query level's final relation to make the plan of, the best by the
 *		main objective of the cost of fetching tuple_fraction of its rows
 *		F(s) (fractional_at) in each scenario.  Penalty(s) is against the
 *		cheapest F(s) of the candidates, the paths of the final relation.
 *		Ties go to the lower expected F, then to the lower F at the point,
 *		the lower startup cost, the earlier path.  'pg' is PostgreSQL's
 *		choice, which stands (NULL is returned) when no candidate has
 *		vectors.  The retain strategy plays no part.
 */
Path *
robust_select_final(RelOptInfo *rel, double tuple_fraction, Path *pg)
{
	int			n = list_length(rel->pathlist);
	int			N = error_sample_count;
	FinalCand  *c;
	double	   *env;
	bool		vectors = false;
	Path	   *chosen = NULL;
	Loss		loss = objective_loss(main_objective_id);
	ListCell   *lc;

	if (n == 0)
		return NULL;
	if (!is_objective(main_objective_id))
		elog(ERROR, "RobDP objective %d is not implemented", main_objective_id);
	foreach(lc, rel->pathlist)
		vectors |= has_vectors((Path *) lfirst(lc));

	c = palloc(sizeof(FinalCand) * n);
	env = palloc(sizeof(double) * N);
	for (int s = 0; s < N; s++)
		env[s] = NAN;
	foreach(lc, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);

		for (int s = 0; s < N; s++)
		{
			double		f = fractional_at(path, tuple_fraction, s);

			if (isnan(env[s]) || f < env[s])
				env[s] = f;
		}
	}
	foreach(lc, rel->pathlist)
	{
		Path	   *path = (Path *) lfirst(lc);
		const RobustPathCost *rc = path->robust;
		FinalCand  *k = &c[foreach_current_index(lc)];
		double		fsum = 0.0;
		double		x[ROBUST_MAX_SAMPLES];

		k->path = path;
		k->pos = foreach_current_index(lc);
		for (int s = 0; s < N; s++)
		{
			double		f = fractional_at(path, tuple_fraction, s);

			fsum += f;
			x[s] = loss_at(loss, f,
						   at(rc ? rc->startup : NULL, path->startup_cost, s),
						   env[s]);
		}
		k->score = loss == LOSS_POINT ?
			fractional_point(path, tuple_fraction) :
			aggregate(objective_agg(main_objective_id), x, N);
		k->mean_frac = fsum / N;
		k->point_frac = fractional_point(path, tuple_fraction);
	}

	if (vectors)
	{
		FinalCand  *best = &c[0];

		for (int i = 1; i < n; i++)
		{
			if (cmp_final(&c[i], best) < 0)
				best = &c[i];
		}
		chosen = best->path;
	}

	if (robust_debug_dump)
	{
		RobustDump	d;
		StringInfoData ids;
		StringInfoData scores;

		initStringInfo(&ids);
		initStringInfo(&scores);
		appendStringInfoChar(&ids, '[');
		appendStringInfoChar(&scores, '[');
		for (int i = 0; i < n; i++)
		{
			if (i > 0)
			{
				appendStringInfoChar(&ids, ',');
				appendStringInfoChar(&scores, ',');
			}
			appendStringInfo(&ids, "%d", c[i].path->robust ? c[i].path->robust->id : -1);
			robust_json_number(&scores, c[i].score);
		}
		appendStringInfoChar(&ids, ']');
		appendStringInfoChar(&scores, ']');
		robust_dump_begin(&d, "final");
		robust_dump_double(&d, "fraction", tuple_fraction);
		robust_dump_int(&d, "objective", main_objective_id);
		robust_dump_bool(&d, "vectors", vectors);
		robust_dump_raw(&d, "cands", ids.data);
		robust_dump_raw(&d, "scores", scores.data);
		robust_dump_int(&d, "chosen", chosen && chosen->robust ? chosen->robust->id : -1);
		robust_dump_int(&d, "pg", pg && pg->robust ? pg->robust->id : -1);
		robust_dump_end(&d);
		pfree(ids.data);
		pfree(scores.data);
	}
	pfree(c);
	pfree(env);
	return chosen;
}
