/*-------------------------------------------------------------------------
 *
 * nodecost_ext.c
 *	  RobDP: per-sample cost vectors for path types without a 2p cost
 *	  function.
 *
 * Only scans, joins and Gather / Gather Merge have per-sample (2p) cost
 * functions (cost_ext.c, joincost_ext.c).  Every other path used to get
 * constant vectors from robust_fill_path_samples(), i.e. it was treated as
 * having no uncertainty even when its inputs had some: a Material over a
 * base relation scan, a Memoize over a parameterized scan, a bitmap heap
 * scan of a relation with an error profile, an Append over partitions.
 *
 * For the node types below the cost is a known function of the inputs'
 * rows and costs, computed by a stock costsize.c function or by a few lines
 * in pathnode.c.  We derive the vectors by doing the same computation once
 * per sample with the inputs' components at that sample, so component s is
 * exactly what PostgreSQL would compute if the cardinalities were those of
 * sample s (the correspondence invariant C1 of the plan, by construction).
 *
 * Some of those computations take arguments the path does not record, such
 * as the limit_tuples of a Sort or a row-count override of an Append.  So
 * before deriving anything we redo the computation at the scalar inputs and
 * require it to reproduce the path's own scalars; if it does not, the
 * derivation is not attempted and the path keeps constant vectors (and is
 * counted by robust_debug_check_costs as having no vector model).
 *
 * Covered: Material, Memoize, Sort, Incremental Sort, Append, Merge Append,
 * Bitmap Heap Scan, and the scans whose cost depends on the row count only
 * through the tlist (Sample, Tid, Tid Range, Function, Table Function,
 * Values, CTE, WorkTable, Named Tuplestore and Result scans), Subquery Scan,
 * the Projection and ProjectSet paths that apply_scanjoin_target_to_paths
 * puts on the final scan/join relation, and the Group Result of a query
 * without a FROM clause.  Not covered: Unique (its row count is an
 * estimate_num_groups() of the input relation), Foreign and Custom scans,
 * joins built by GEQO (costed by the stock functions), and upper-planning
 * paths.
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/nodecost_ext.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "miscadmin.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/cost_ext.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "optimizer/sample.h"
#include "utils/memutils.h"

/* Largest relative difference at which a recomputed scalar still matches */
#define DERIVE_TOLERANCE	1e-9

/* Component i of a vector, or the scalar if there is none */
static inline double
vec_at(const Sample *v, int i, double scalar)
{
	if (v == NULL || v->sample_count <= 0)
		return scalar;
	if (v->sample_count == 1)
		return v->sample[0];
	Assert(i < v->sample_count);
	return v->sample[i];
}

/* Is this vector longer than 1? */
static inline bool
varies(const Sample *v)
{
	return v != NULL && v->sample_count > 1;
}

/* Do any of the path's vectors vary? */
static bool
path_varies(const Path *path)
{
	return varies(path->rows_sample) ||
		varies(path->startup_cost_sample) ||
		varies(path->total_cost_sample);
}

/* Do a recomputed value and the path's value agree? */
static bool
same_value(double a, double b)
{
	double		scale = Max(fabs(a), fabs(b));

	return fabs(a - b) <= DERIVE_TOLERANCE * Max(scale, 1.0);
}

/* Does a recomputation reproduce the path's scalars? */
static bool
same_scalars(const Path *path, double rows, Cost startup_cost,
			 Cost total_cost)
{
	return same_value(path->rows, rows) &&
		same_value(path->startup_cost, startup_cost) &&
		same_value(path->total_cost, total_cost);
}

/* Set the path's header fields to their components at sample i */
static void
project_header(Path *dst, const Path *src, int i)
{
	dst->rows = vec_at(src->rows_sample, i, src->rows);
	dst->startup_cost = vec_at(src->startup_cost_sample, i, src->startup_cost);
	dst->total_cost = vec_at(src->total_cost_sample, i, src->total_cost);
}

/*
 * Output vectors under construction.  "n" is 1 when no input varies; the
 * vectors are then constants equal to the path's scalars.
 */
typedef struct DerivedVectors
{
	int			n;
	Sample	   *rows;
	Sample	   *startup;
	Sample	   *total;
} DerivedVectors;

static void
derived_init(DerivedVectors *dv, bool any_input_varies)
{
	dv->n = any_input_varies ? error_sample_count : 1;
	dv->rows = initialize_sample(dv->n);
	dv->startup = initialize_sample(dv->n);
	dv->total = initialize_sample(dv->n);
}

static inline void
derived_set(DerivedVectors *dv, int i, double rows, Cost startup_cost,
			Cost total_cost)
{
	dv->rows->sample[i] = rows;
	dv->startup->sample[i] = startup_cost;
	dv->total->sample[i] = total_cost;
}

/* Attach the vectors; a constant result takes the path's own scalars */
static void
derived_attach(Path *path, DerivedVectors *dv)
{
	if (dv->n == 1)
		derived_set(dv, 0, path->rows, path->startup_cost, path->total_cost);
	path->rows_sample = dv->rows;
	path->startup_cost_sample = dv->startup;
	path->total_cost_sample = dv->total;
}

/*
 * Material: cost_material() over the subpath, as create_material_path.
 */
static bool
derive_material(PlannerInfo *root, MaterialPath *mpath)
{
	Path	   *sub = mpath->subpath;
	int			width = sub->pathtarget->width;
	DerivedVectors dv;
	Path		tmp;

	robust_fill_path_samples(root, sub);

	cost_material(&tmp, sub->startup_cost, sub->total_cost, sub->rows, width);
	if (!same_scalars(&mpath->path, tmp.rows, tmp.startup_cost, tmp.total_cost))
		return false;

	derived_init(&dv, path_varies(sub));
	for (int i = 0; i < dv.n && dv.n > 1; i++)
	{
		cost_material(&tmp,
					  vec_at(sub->startup_cost_sample, i, sub->startup_cost),
					  vec_at(sub->total_cost_sample, i, sub->total_cost),
					  vec_at(sub->rows_sample, i, sub->rows),
					  width);
		derived_set(&dv, i, tmp.rows, tmp.startup_cost, tmp.total_cost);
	}
	derived_attach(&mpath->path, &dv);
	return true;
}

/*
 * Memoize: the subpath's costs plus cpu_tuple_cost for caching the first
 * entry, as create_memoize_path.  (Its rescan cost is computed per sample by
 * the nestloop, from the subpath's vectors and calls_sample.)
 */
static bool
derive_memoize(PlannerInfo *root, MemoizePath *mpath)
{
	Path	   *sub = mpath->subpath;
	DerivedVectors dv;

	robust_fill_path_samples(root, sub);

	if (!same_scalars(&mpath->path, sub->rows,
					  sub->startup_cost + cpu_tuple_cost,
					  sub->total_cost + cpu_tuple_cost))
		return false;

	derived_init(&dv, path_varies(sub));
	for (int i = 0; i < dv.n && dv.n > 1; i++)
		derived_set(&dv, i,
					vec_at(sub->rows_sample, i, sub->rows),
					vec_at(sub->startup_cost_sample, i, sub->startup_cost) + cpu_tuple_cost,
					vec_at(sub->total_cost_sample, i, sub->total_cost) + cpu_tuple_cost);
	derived_attach(&mpath->path, &dv);
	return true;
}

/*
 * Sort: cost_sort() over the subpath, as create_sort_path.  The path does
 * not record limit_tuples; the scalar check accepts only paths built
 * without a limit (all of those in the join search are).
 */
static bool
derive_sort(PlannerInfo *root, SortPath *spath)
{
	Path	   *sub = spath->subpath;
	List	   *pathkeys = spath->path.pathkeys;
	int			width = sub->pathtarget->width;
	DerivedVectors dv;
	Path		tmp;

	robust_fill_path_samples(root, sub);

	cost_sort(&tmp, root, pathkeys, sub->total_cost, sub->rows, width,
			  0.0, work_mem, -1.0);
	if (!same_scalars(&spath->path, tmp.rows, tmp.startup_cost, tmp.total_cost))
		return false;

	derived_init(&dv, path_varies(sub));
	for (int i = 0; i < dv.n && dv.n > 1; i++)
	{
		cost_sort(&tmp, root, pathkeys,
				  vec_at(sub->total_cost_sample, i, sub->total_cost),
				  vec_at(sub->rows_sample, i, sub->rows),
				  width, 0.0, work_mem, -1.0);
		derived_set(&dv, i, tmp.rows, tmp.startup_cost, tmp.total_cost);
	}
	derived_attach(&spath->path, &dv);
	return true;
}

/*
 * Incremental Sort: cost_incremental_sort() over the subpath, as
 * create_incremental_sort_path; limit_tuples as for Sort.
 */
static bool
derive_incremental_sort(PlannerInfo *root, IncrementalSortPath *ipath)
{
	Path	   *sub = ipath->spath.subpath;
	List	   *pathkeys = ipath->spath.path.pathkeys;
	int			presorted = ipath->nPresortedCols;
	int			width = sub->pathtarget->width;
	DerivedVectors dv;
	Path		tmp;

	robust_fill_path_samples(root, sub);

	cost_incremental_sort(&tmp, root, pathkeys, presorted,
						  sub->startup_cost, sub->total_cost, sub->rows,
						  width, 0.0, work_mem, -1.0);
	if (!same_scalars(&ipath->spath.path, tmp.rows, tmp.startup_cost,
					  tmp.total_cost))
		return false;

	derived_init(&dv, path_varies(sub));
	for (int i = 0; i < dv.n && dv.n > 1; i++)
	{
		cost_incremental_sort(&tmp, root, pathkeys, presorted,
							  vec_at(sub->startup_cost_sample, i, sub->startup_cost),
							  vec_at(sub->total_cost_sample, i, sub->total_cost),
							  vec_at(sub->rows_sample, i, sub->rows),
							  width, 0.0, work_mem, -1.0);
		derived_set(&dv, i, tmp.rows, tmp.startup_cost, tmp.total_cost);
	}
	derived_attach(&ipath->spath.path, &dv);
	return true;
}

/*
 * Give every subpath of an Append / Merge Append its vectors, and say
 * whether any of them varies.  (In the caller's memory context: the vectors
 * belong to the subpaths.)
 */
static bool
fill_subpaths(PlannerInfo *root, List *subpaths)
{
	bool		any_varies = false;
	ListCell   *lc;

	foreach(lc, subpaths)
	{
		Path	   *sub = (Path *) lfirst(lc);

		robust_fill_path_samples(root, sub);
		if (path_varies(sub))
			any_varies = true;
	}
	return any_varies;
}

/*
 * Make header copies of the subpaths, for projecting them one sample at a
 * time.  cost_append and the Merge Append input costs read only the Path
 * fields of the subpaths.
 */
static List *
copy_subpath_headers(List *subpaths)
{
	List	   *copies = NIL;
	ListCell   *lc;

	foreach(lc, subpaths)
	{
		Path	   *copy = palloc(sizeof(Path));

		*copy = *(const Path *) lfirst(lc);
		copies = lappend(copies, copy);
	}
	return copies;
}

/* Set every header copy to its subpath's components at sample i */
static void
project_subpath_headers(List *copies, List *subpaths, int i)
{
	ListCell   *lc1,
			   *lc2;

	forboth(lc1, copies, lc2, subpaths)
		project_header((Path *) lfirst(lc1), (const Path *) lfirst(lc2), i);
}

/*
 * Append: cost_append() over the subpaths, as create_append_path, including
 * its shortcut for a single child of the same parallel awareness.  A
 * caller-supplied row count (the "rows" argument) is not recorded; the
 * scalar check rejects such paths.
 */
static bool
derive_append(PlannerInfo *root, AppendPath *apath)
{
	List	   *subpaths = apath->subpaths;
	DerivedVectors dv;
	AppendPath	copy;
	List	   *headers;
	bool		any_varies;
	MemoryContext cxt,
				oldcxt;

	if (list_length(subpaths) == 1 &&
		((Path *) linitial(subpaths))->parallel_aware ==
		apath->path.parallel_aware)
	{
		Path	   *child = (Path *) linitial(subpaths);

		robust_fill_path_samples(root, child);
		if (!same_scalars(&apath->path, child->rows, child->startup_cost,
						  child->total_cost))
			return false;
		derived_init(&dv, path_varies(child));
		for (int i = 0; i < dv.n && dv.n > 1; i++)
			derived_set(&dv, i,
						vec_at(child->rows_sample, i, child->rows),
						vec_at(child->startup_cost_sample, i, child->startup_cost),
						vec_at(child->total_cost_sample, i, child->total_cost));
		derived_attach(&apath->path, &dv);
		return true;
	}

	/*
	 * cost_append on a copy, at the scalar inputs first.  With a single
	 * child, create_append_path calls cost_append before it sets the
	 * child's pathkeys on the Append (cost_append asserts that a
	 * parallel-aware Append is unordered), so the costing saw the caller's
	 * pathkeys; those are not recorded, but they were either NIL or, for a
	 * non-parallel Append, the ones the path has.  Try them in that order.
	 */
	any_varies = fill_subpaths(root, subpaths);
	copy = *apath;
	if (list_length(subpaths) == 1)
		copy.path.pathkeys = NIL;
	cost_append(&copy);
	if (!same_scalars(&apath->path, copy.path.rows, copy.path.startup_cost,
					  copy.path.total_cost))
	{
		if (list_length(subpaths) != 1 || apath->path.parallel_aware ||
			apath->path.pathkeys == NIL)
			return false;
		copy.path.pathkeys = apath->path.pathkeys;
		cost_append(&copy);
		if (!same_scalars(&apath->path, copy.path.rows,
						  copy.path.startup_cost, copy.path.total_cost))
			return false;
	}

	derived_init(&dv, any_varies);
	if (dv.n > 1)
	{
		cxt = AllocSetContextCreate(CurrentMemoryContext,
									"RobDP append vectors",
									ALLOCSET_SMALL_SIZES);
		oldcxt = MemoryContextSwitchTo(cxt);
		headers = copy_subpath_headers(subpaths);
		MemoryContextSwitchTo(oldcxt);

		copy.subpaths = headers;
		for (int i = 0; i < dv.n; i++)
		{
			project_subpath_headers(headers, subpaths, i);
			cost_append(&copy);
			derived_set(&dv, i, copy.path.rows, copy.path.startup_cost,
						copy.path.total_cost);
		}
		MemoryContextDelete(cxt);
	}
	derived_attach(&apath->path, &dv);
	return true;
}

/*
 * Merge Append: the input costs (each subpath, or a Sort over it) and
 * cost_merge_append(), as create_merge_append_path.
 */
static void
merge_append_cost(PlannerInfo *root, const MergeAppendPath *mpath,
				  List *subpaths, double *rows, Cost *startup_cost,
				  Cost *total_cost)
{
	List	   *pathkeys = mpath->path.pathkeys;
	Cost		input_startup_cost = 0;
	Cost		input_total_cost = 0;
	double		tuples = 0;
	ListCell   *lc;
	Path		tmp;

	foreach(lc, subpaths)
	{
		Path	   *sub = (Path *) lfirst(lc);

		tuples += sub->rows;
		if (pathkeys_contained_in(pathkeys, sub->pathkeys))
		{
			input_startup_cost += sub->startup_cost;
			input_total_cost += sub->total_cost;
		}
		else
		{
			cost_sort(&tmp, root, pathkeys, sub->total_cost, sub->rows,
					  sub->pathtarget->width, 0.0, work_mem,
					  mpath->limit_tuples);
			input_startup_cost += tmp.startup_cost;
			input_total_cost += tmp.total_cost;
		}
	}

	*rows = tuples;
	if (list_length(subpaths) == 1 &&
		((Path *) linitial(subpaths))->parallel_aware ==
		mpath->path.parallel_aware)
	{
		*startup_cost = input_startup_cost;
		*total_cost = input_total_cost;
	}
	else
	{
		cost_merge_append(&tmp, root, pathkeys, list_length(subpaths),
						  input_startup_cost, input_total_cost, tuples);
		*startup_cost = tmp.startup_cost;
		*total_cost = tmp.total_cost;
	}
}

static bool
derive_merge_append(PlannerInfo *root, MergeAppendPath *mpath)
{
	List	   *subpaths = mpath->subpaths;
	DerivedVectors dv;
	bool		any_varies;
	double		rows;
	Cost		startup_cost,
				total_cost;

	any_varies = fill_subpaths(root, subpaths);
	merge_append_cost(root, mpath, subpaths, &rows, &startup_cost,
					  &total_cost);
	if (!same_scalars(&mpath->path, rows, startup_cost, total_cost))
		return false;

	derived_init(&dv, any_varies);
	if (dv.n > 1)
	{
		MemoryContext cxt;
		MemoryContext oldcxt;
		List	   *headers;

		cxt = AllocSetContextCreate(CurrentMemoryContext,
									"RobDP merge append vectors",
									ALLOCSET_SMALL_SIZES);
		oldcxt = MemoryContextSwitchTo(cxt);
		headers = copy_subpath_headers(subpaths);
		MemoryContextSwitchTo(oldcxt);

		for (int i = 0; i < dv.n; i++)
		{
			project_subpath_headers(headers, subpaths, i);
			merge_append_cost(root, mpath, headers, &rows, &startup_cost,
							  &total_cost);
			derived_set(&dv, i, rows, startup_cost, total_cost);
		}
		MemoryContextDelete(cxt);
	}
	derived_attach(&mpath->path, &dv);
	return true;
}

/*
 * Scans whose row count comes from the relation (or the ParamPathInfo) and
 * whose cost depends on it only through the tlist cost per output row:
 *
 *		rows[s]  = source rows[s], per worker if the scan is partial
 *		total[s] = total + tlist_per_tuple * (rows[s] - rows)
 *		startup[s] = startup
 *
 * This is how cost_bitmap_heap_scan, cost_samplescan, cost_tidscan,
 * cost_tidrangescan, cost_functionscan, cost_tablefuncscan, cost_valuesscan
 * and cost_ctescan use the row count; cost_namedtuplestorescan and
 * cost_resultscan do not charge a tlist cost at all (tlist_per_tuple is 0).
 * The other terms (pages, tuples fetched, quals) depend on the relation's
 * size and the selectivities only, which have no vectors.  The scalar check
 * makes sure the path's rows are the ones this formula starts from.
 */
static bool
derive_rows_only_scan(Path *path, bool charges_tlist)
{
	const Sample *src;
	double		src_rows;
	double		divisor = 1.0;
	Cost		per_tuple = charges_tlist ? path->pathtarget->cost.per_tuple : 0;
	DerivedVectors dv;

	if (path->param_info)
	{
		src = path->param_info->ppi_rows_sample;
		src_rows = path->param_info->ppi_rows;
	}
	else
	{
		src = path->parent->rows_sample;
		src_rows = path->parent->rows;
	}

	if (path->parallel_workers > 0)
	{
		/* only bitmap heap scans are costed per worker here */
		if (path->pathtype != T_BitmapHeapScan)
			return false;
		divisor = get_parallel_divisor(path);
		src_rows = clamp_row_est(src_rows / divisor);
	}
	if (!same_value(path->rows, src_rows))
		return false;

	derived_init(&dv, varies(src));
	for (int i = 0; i < dv.n && dv.n > 1; i++)
	{
		double		rows_i = vec_at(src, i, src_rows);

		if (path->parallel_workers > 0)
			rows_i = clamp_row_est(rows_i / divisor);
		derived_set(&dv, i, rows_i, path->startup_cost,
					path->total_cost + per_tuple * (rows_i - path->rows));
	}
	derived_attach(path, &dv);
	return true;
}

/*
 * Subquery Scan: cost_subqueryscan() over the subquery's path, as
 * create_subqueryscan_path.  The rows are the subpath's rows times the
 * selectivity of the relation's quals, so they vary with the subpath.  The
 * subpath usually has no vectors (it is an upper-planning path of another
 * query level), but it does when the subquery is a bare scan or join whose
 * paths reach its final relation unchanged.
 *
 * The path does not record trivial_pathtarget; we take whichever value
 * reproduces the scalars (when the relation has quals both give the same
 * cost).  cost_subqueryscan reads only the Path fields of the subpath, so a
 * header copy can be projected sample by sample.
 */
static bool
derive_subquery_scan(PlannerInfo *root, SubqueryScanPath *spath)
{
	Path	   *sub = spath->subpath;
	RelOptInfo *rel = spath->path.parent;
	ParamPathInfo *ppi = spath->path.param_info;
	SubqueryScanPath copy;
	Path		subhdr;
	bool		trivial_pathtarget;
	DerivedVectors dv;
	MemoryContext cxt,
				oldcxt;

	robust_fill_path_samples(root, sub);

	copy = *spath;
	subhdr = *sub;
	copy.subpath = &subhdr;
	trivial_pathtarget = false;
	cost_subqueryscan(&copy, root, rel, ppi, trivial_pathtarget);
	if (!same_scalars(&spath->path, copy.path.rows, copy.path.startup_cost,
					  copy.path.total_cost))
	{
		trivial_pathtarget = true;
		cost_subqueryscan(&copy, root, rel, ppi, trivial_pathtarget);
		if (!same_scalars(&spath->path, copy.path.rows,
						  copy.path.startup_cost, copy.path.total_cost))
			return false;
	}

	derived_init(&dv, path_varies(sub));
	if (dv.n > 1)
	{
		/* cost_subqueryscan copies the quals of a parameterized path */
		cxt = AllocSetContextCreate(CurrentMemoryContext,
									"RobDP subquery scan vectors",
									ALLOCSET_SMALL_SIZES);
		oldcxt = MemoryContextSwitchTo(cxt);
		for (int i = 0; i < dv.n; i++)
		{
			project_header(&subhdr, sub, i);
			cost_subqueryscan(&copy, root, rel, ppi, trivial_pathtarget);
			derived_set(&dv, i, copy.path.rows, copy.path.startup_cost,
						copy.path.total_cost);
		}
		MemoryContextSwitchTo(oldcxt);
		MemoryContextDelete(cxt);
	}
	derived_attach(&spath->path, &dv);
	return true;
}

/*
 * Projection: the subpath plus the cost of evaluating the new target, as
 * create_projection_path.  A dummy projection (one createplan.c folds into
 * the subpath's plan node) charges only the difference between the new
 * target's cost and the subpath's, and no cpu_tuple_cost.  The subpath's
 * target is the "oldtarget" create_projection_path saw; the scalar check
 * makes sure it has not changed since.
 */
static bool
derive_projection(PlannerInfo *root, ProjectionPath *ppath)
{
	Path	   *sub = ppath->subpath;
	PathTarget *target = ppath->path.pathtarget;
	PathTarget *oldtarget = sub->pathtarget;
	Cost		startup_delta;
	Cost		per_tuple;
	DerivedVectors dv;

	robust_fill_path_samples(root, sub);

	if (ppath->dummypp)
	{
		startup_delta = target->cost.startup - oldtarget->cost.startup;
		per_tuple = target->cost.per_tuple - oldtarget->cost.per_tuple;
	}
	else
	{
		startup_delta = target->cost.startup;
		per_tuple = cpu_tuple_cost + target->cost.per_tuple;
	}

	if (!same_scalars(&ppath->path, sub->rows,
					  sub->startup_cost + startup_delta,
					  sub->total_cost + startup_delta + per_tuple * sub->rows))
		return false;

	derived_init(&dv, path_varies(sub));
	for (int i = 0; i < dv.n && dv.n > 1; i++)
	{
		double		rows_i = vec_at(sub->rows_sample, i, sub->rows);

		derived_set(&dv, i, rows_i,
					vec_at(sub->startup_cost_sample, i, sub->startup_cost) +
					startup_delta,
					vec_at(sub->total_cost_sample, i, sub->total_cost) +
					startup_delta + per_tuple * rows_i);
	}
	derived_attach(&ppath->path, &dv);
	return true;
}

/*
 * ProjectSet: the subpath's rows times the largest set-returning factor of
 * the target, plus per-row costs, as create_set_projection_path.  (Like
 * the stock code, the output rows are not clamped.)
 */
static void
project_set_cost(const PathTarget *target, double tlist_rows,
				 double sub_rows, Cost sub_startup, Cost sub_total,
				 double *rows, Cost *startup_cost, Cost *total_cost)
{
	*rows = sub_rows * tlist_rows;
	*startup_cost = sub_startup + target->cost.startup;
	*total_cost = sub_total + target->cost.startup +
		(cpu_tuple_cost + target->cost.per_tuple) * sub_rows +
		(*rows - sub_rows) * cpu_tuple_cost / 2;
}

static bool
derive_project_set(PlannerInfo *root, ProjectSetPath *ppath)
{
	Path	   *sub = ppath->subpath;
	PathTarget *target = ppath->path.pathtarget;
	double		tlist_rows = 1;
	double		rows;
	Cost		startup_cost;
	Cost		total_cost;
	DerivedVectors dv;
	ListCell   *lc;

	robust_fill_path_samples(root, sub);

	foreach(lc, target->exprs)
	{
		double		itemrows;

		itemrows = expression_returns_set_rows(root, (Node *) lfirst(lc));
		if (tlist_rows < itemrows)
			tlist_rows = itemrows;
	}

	project_set_cost(target, tlist_rows, sub->rows, sub->startup_cost,
					 sub->total_cost, &rows, &startup_cost, &total_cost);
	if (!same_scalars(&ppath->path, rows, startup_cost, total_cost))
		return false;

	derived_init(&dv, path_varies(sub));
	for (int i = 0; i < dv.n && dv.n > 1; i++)
	{
		project_set_cost(target, tlist_rows,
						 vec_at(sub->rows_sample, i, sub->rows),
						 vec_at(sub->startup_cost_sample, i, sub->startup_cost),
						 vec_at(sub->total_cost_sample, i, sub->total_cost),
						 &rows, &startup_cost, &total_cost);
		derived_set(&dv, i, rows, startup_cost, total_cost);
	}
	derived_attach(&ppath->path, &dv);
	return true;
}

/*
 * Group Result on the relation of an empty FROM clause: query_planner adds
 * it directly to that RTE_RESULT base relation.  create_group_result_path
 * sets rows = 1 and costs from the target and the quals only, so it has no
 * input that could vary and its exact vectors are constants.
 */
static bool
derive_group_result(GroupResultPath *gpath)
{
	DerivedVectors dv;

	if (gpath->path.parent->rtekind != RTE_RESULT)
		return false;
	derived_init(&dv, false);
	derived_attach(&gpath->path, &dv);
	return true;
}

/*
 * robust_derive_path_samples
 *		Give "path" per-sample vectors derived from its inputs, if its node
 *		type is covered.
 *
 * Returns false, leaving the path alone, if the type is not covered or the
 * path does not pass the scalar check; the caller then attaches constant
 * vectors.  Only called for paths of base and join relations during the
 * vector passes (robust_fill_path_samples checks that).
 */
bool
robust_derive_path_samples(PlannerInfo *root, Path *path)
{
	switch (nodeTag(path))
	{
		case T_MaterialPath:
			return derive_material(root, (MaterialPath *) path);
		case T_MemoizePath:
			return derive_memoize(root, (MemoizePath *) path);
		case T_SortPath:
			return derive_sort(root, (SortPath *) path);
		case T_IncrementalSortPath:
			return derive_incremental_sort(root, (IncrementalSortPath *) path);
		case T_AppendPath:
			return derive_append(root, (AppendPath *) path);
		case T_MergeAppendPath:
			return derive_merge_append(root, (MergeAppendPath *) path);
		case T_BitmapHeapPath:
			return derive_rows_only_scan(path, true);
		case T_TidPath:
		case T_TidRangePath:
			return derive_rows_only_scan(path, true);
		case T_SubqueryScanPath:
			return derive_subquery_scan(root, (SubqueryScanPath *) path);
		case T_ProjectionPath:
			return derive_projection(root, (ProjectionPath *) path);
		case T_ProjectSetPath:
			return derive_project_set(root, (ProjectSetPath *) path);
		case T_GroupResultPath:
			return derive_group_result((GroupResultPath *) path);
		case T_Path:
			switch (path->pathtype)
			{
				case T_SampleScan:
				case T_FunctionScan:
				case T_TableFuncScan:
				case T_ValuesScan:
				case T_CteScan:
				case T_WorkTableScan:
					return derive_rows_only_scan(path, true);
				case T_NamedTuplestoreScan:
					return derive_rows_only_scan(path, false);
				case T_Result:
					/* a scan of an RTE_RESULT relation (cost_resultscan) */
					if (path->parent->rtekind == RTE_RESULT)
						return derive_rows_only_scan(path, false);
					return false;
				default:
					return false;
			}
		default:
			return false;
	}
}
