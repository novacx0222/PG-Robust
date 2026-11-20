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
	return startup + f * (total - startup);
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
	return path->startup_cost + f * (path->total_cost - path->startup_cost);
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
	ListCell   *lc;

	if (n == 0)
		return NULL;
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
		double		m = 0.0;
		double		sum = 0.0;
		double		fsum = 0.0;

		k->path = path;
		k->pos = foreach_current_index(lc);
		for (int s = 0; s < N; s++)
		{
			double		f = fractional_at(path, tuple_fraction, s);
			double		x = 0.0;

			fsum += f;
			switch (main_objective_id)
			{
				case OBJ_WORST_PENALTY:
				case OBJ_EXPECTED_PENALTY:
					x = f > env[s] * (1.0 + robust_penalty_tau) ? f - env[s] : 0.0;
					break;
				case OBJ_WORST_TOTAL:
				case OBJ_EXPECTED_TOTAL:
					x = f;
					break;
				case OBJ_WORST_STARTUP:
				case OBJ_EXPECTED_STARTUP:
					x = at(rc ? rc->startup : NULL, path->startup_cost, s);
					break;
				default:
					elog(ERROR, "RobDP objective %d is not implemented",
						 main_objective_id);
			}
			sum += x;
			if (x > m)
				m = x;
		}
		k->score = (main_objective_id == OBJ_WORST_PENALTY ||
					main_objective_id == OBJ_WORST_TOTAL ||
					main_objective_id == OBJ_WORST_STARTUP) ? m : sum / N;
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
