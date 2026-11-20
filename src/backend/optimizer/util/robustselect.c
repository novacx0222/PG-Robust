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

#include <math.h>

#include "nodes/bitmapset.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/robust.h"
#include "optimizer/robustcost.h"
#include "optimizer/robustselect.h"
#include "utils/guc.h"
#include "utils/json.h"

/* The objectives implemented, in v20's numbering */
#define OBJ_WORST_PENALTY		0
#define OBJ_EXPECTED_PENALTY	1
#define OBJ_WORST_TOTAL			2
#define OBJ_EXPECTED_TOTAL		3
#define OBJ_WORST_STARTUP		4
#define OBJ_EXPECTED_STARTUP	5
#define OBJ_LAST_IMPLEMENTED	OBJ_EXPECTED_STARTUP

/* Why a path was kept (the "by" of the keep event) */
typedef enum KeptBy
{
	KEPT_NOT = 0,
	KEPT_MAIN,
	KEPT_RETAIN,
	KEPT_UNPARAM,
} KeptBy;

typedef struct Cand
{
	Path	   *path;
	int			pos;			/* position among the candidates */
	double		main;			/* main objective */
	double		retain;			/* retain strategy */
	double		mean_startup;
	double		mean_total;
	KeptBy		by;
} Cand;

/* --------------------------------------------------------------- GUC */

bool
check_robust_objective(int *newval, void **extra, GucSource source)
{
	if (*newval > OBJ_LAST_IMPLEMENTED)
	{
		GUC_check_errdetail("Objective %d is not implemented; objectives 0 to %d are.",
							*newval, OBJ_LAST_IMPLEMENTED);
		return false;
	}
	return true;
}

/* Does the objective compare paths to the envelope? */
bool
robust_objective_needs_envelope(int objective)
{
	return objective == OBJ_WORST_PENALTY ||
		objective == OBJ_EXPECTED_PENALTY;
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
 * Mean and maximum of a vector, the scalar standing for a vector of N
 * copies.  The sums run over s = 0 .. N-1 in order, so the test harness
 * computes the same doubles.  The maximum starts at 0, as in v20.
 */
static double
vec_mean(const RobustVec *v, double scalar)
{
	double		sum = 0.0;

	for (int s = 0; s < error_sample_count; s++)
		sum += at(v, scalar, s);
	return sum / error_sample_count;
}

static double
vec_max(const RobustVec *v, double scalar)
{
	double		m = 0.0;

	for (int s = 0; s < error_sample_count; s++)
	{
		double		x = at(v, scalar, s);

		if (x > m)
			m = x;
	}
	return m;
}

/*
 * The penalty in scenario s against the envelope C*(s): total(s) - C*(s)
 * beyond the tolerance, else 0.  No envelope (NULL, or NaN in s): 0.
 */
static double
penalty(Path *path, const double *envelope, int s)
{
	const RobustVec *tv = path->robust ? path->robust->total : NULL;
	double		c = at(tv, path->total_cost, s);
	double		e;
	double		thresh;

	if (envelope == NULL || isnan(envelope[s]))
		return 0.0;
	e = envelope[s];
	thresh = e * (1.0 + robust_penalty_tau);
	if (c > thresh)
		return c - e;
	return 0.0;
}

static double
objective(int id, Path *path, const double *envelope)
{
	const RobustPathCost *rc = path->robust;
	double		m = 0.0;
	double		sum = 0.0;

	switch (id)
	{
		case OBJ_WORST_PENALTY:
			for (int s = 0; s < error_sample_count; s++)
			{
				double		p = penalty(path, envelope, s);

				if (p > m)
					m = p;
			}
			return m;
		case OBJ_EXPECTED_PENALTY:
			for (int s = 0; s < error_sample_count; s++)
				sum += penalty(path, envelope, s);
			return sum / error_sample_count;
		case OBJ_WORST_TOTAL:
			return vec_max(rc ? rc->total : NULL, path->total_cost);
		case OBJ_EXPECTED_TOTAL:
			return vec_mean(rc ? rc->total : NULL, path->total_cost);
		case OBJ_WORST_STARTUP:
			return vec_max(rc ? rc->startup : NULL, path->startup_cost);
		case OBJ_EXPECTED_STARTUP:
			return vec_mean(rc ? rc->startup : NULL, path->startup_cost);
	}
	elog(ERROR, "RobDP objective %d is not implemented", id);
	return 0.0;					/* keep compiler quiet */
}

/* -------------------------------------------------------- candidates */

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

/*
 * robust_select_offer
 *		add_path() / add_partial_path() in the main pass of a selecting
 *		search: append the candidate, unless the list has its twin.  Returns
 *		whether it was appended.
 */
bool
robust_select_offer(List **pathlist, Path *path)
{
	ListCell   *lc;

	foreach(lc, *pathlist)
	{
		if (same_candidate((Path *) lfirst(lc), path))
			return false;
	}
	*pathlist = lappend(*pathlist, path);
	return true;
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
		  int retain, const double *envelope, Cand *c, int n, Cand **kept,
		  int nkept)
{
	RobustDump	d;
	StringInfoData buf;

	robust_dump_begin(&d, "keep");
	robust_dump_str(&d, "pass", robust_pass_name(root->robust_pass));
	robust_dump_relids(&d, "rels", root, rel->relids);
	robust_dump_bool(&d, "partial", partial);
	robust_dump_int(&d, "limit", limit);
	robust_dump_int(&d, "retain", retain);
	robust_dump_bool(&d, "envelope", envelope != NULL);
	dump_ids_scores(&d, "cands", c, n, true, false);
	dump_ids_scores(&d, "main", c, n, false, false);
	if (retain > 0)
		dump_ids_scores(&d, "retain_score", c, n, false, true);
	initStringInfo(&buf);
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

/*
 * robust_select_paths
 *		Keep the paths of rel's pathlist (or partial_pathlist) the objectives
 *		select, scored, in the order of the main objective: 'limit' by the
 *		main objective, then 'retain' by the retain strategy, then (pathlist
 *		only) the best unparameterized candidate if none was kept.  A limit
 *		of at least the number of candidates keeps them all: the relation is
 *		only scored.  'envelope' is the relation's, for its complete or its
 *		partial paths, or NULL.
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

	if (n == 0)
		return;
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
		c[i].retain = retain > 0 ?
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
		qsort(rest, nrest, sizeof(Cand *), cmp_retain);
		for (int i = 0; i < nrest && i < retain; i++)
			rest[i]->by = KEPT_RETAIN;
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
		dump_keep(root, rel, partial, limit, retain, envelope, c, n, kept,
				  nkept);

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
