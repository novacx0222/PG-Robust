/*-------------------------------------------------------------------------
 *
 * cost_check.c
 *	  Debug-only cross check of the RobDP 1p and 2p cost functions.
 *
 * RobDP keeps two implementations of every cost function that has a
 * per-sample variant (cost_ext.h): the *_1p function computes scalar costs
 * at one error sample and is what pass 1 runs, and the *_2p function
 * computes per-sample cost vectors and is what passes 0 and 2 run.  The two
 * must agree sample by sample:
 *
 *		2p(path)[s] == 1p(path, with its inputs projected onto sample s)
 *
 * "Projected onto sample s" means that every scalar the 1p function reads
 * and that has a vector counterpart is replaced by the s-th component of
 * that vector: rows, startup_cost and total_cost of the input paths, rows of
 * the path's relation and ppi_rows of its ParamPathInfo.  A vector of length
 * 1 is a constant; its single component is used for every s.
 *
 * When robust_debug_check_costs is on, costsize.c calls one of the
 * robust_check_* functions below right after each 2p cost function returns.
 * For each sample s the check makes scratch copies of the path and of its
 * direct inputs with the projected scalars, sets root->pass = 1 and
 * root->round = s (the environment pass 1 runs the 1p functions in), calls
 * the 1p function(s) on the copies, and compares the resulting rows,
 * startup_cost and total_cost with the s-th components of the path's
 * vectors.  For joins it runs initial_cost_*_1p and final_cost_*_1p, since
 * the 2p workspace cannot be fed to the 1p final function.
 *
 * The check never changes the path being checked, its inputs, or any
 * planning decision: all scratch data lives in a temporary memory context
 * that is deleted before returning.  Caches the cost functions keep in
 * RestrictInfos (eval_cost, scansel_cache, hash bucket statistics) are
 * shared with normal planning; the 1p functions only fill them when they are
 * empty, and the 2p function has already run, so the check adds no new
 * entries in practice.
 *
 * Reporting:
 *
 * - For each checked path and each field that differs in at least one
 *	 sample, one LOG line naming the cost function, the path, the number of
 *	 differing samples, the first differing sample with both values, and the
 *	 largest relative difference.  Values agree when they differ by at most
 *	 ROBUST_CHECK_TOLERANCE relative to the larger one (or absolutely, for
 *	 values below 1).
 * - At the end of subquery_planner, one WARNING per query level that had
 *	 differences, with per-function counts in the DETAIL, and one LOG line
 *	 with the number of checked paths and the paths that got constant
 *	 vectors because their node type has no 2p cost function
 *	 (robust_fill_path_samples), and one LOG line comparing the pass-2 paths
 *	 with the pass-1 envelope (robust_check_envelope).
 *
 * Projection depth: the direct inputs, plus the fields below them that the
 * cost functions read through cost_rescan: num_batches of a HashPath (from
 * num_batches_sample), and the subpath and calls of a MemoizePath (from
 * calls_sample), plus the rows of the base relations of a Memoize's cache
 * keys, which estimate_num_groups reads (robust_project_base_rows).  Nodes
 * further down keep their scalar fields; no cost
 * function reads them.
 *
 * Structural decisions are frozen: the 2p mergejoin decides
 * materialize_inner once for all samples, and the 1p function is run with
 * that decision (robust_mergejoin_frozen_materialize) instead of making its
 * own, so the costs of the same plan shape are compared.
 *
 * Cost: one check runs the 1p function(s) error_sample_count times, so this
 * is for debugging and regression runs only.
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/path/cost_check.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "optimizer/cost.h"
#include "optimizer/cost_check.h"
#include "optimizer/cost_ext.h"
#include "optimizer/optimizer.h"
#include "optimizer/paths.h"
#include "optimizer/sample.h"
#include "parser/parsetree.h"
#include "utils/float.h"
#include "utils/memutils.h"

/* GUC variable */
bool		robust_debug_check_costs = false;

/* Largest relative difference that still counts as agreement */
#define ROBUST_CHECK_TOLERANCE	1e-9

/* Most distinct node types tracked in the constant-vector counts */
#define ROBUST_CHECK_MAX_FILL_TYPES 32

/* The cost functions that have a 2p version */
typedef enum RobustCheckFunc
{
	RCF_SEQSCAN,
	RCF_INDEX,
	RCF_GATHER,
	RCF_GATHER_MERGE,
	RCF_NESTLOOP,
	RCF_MERGEJOIN,
	RCF_HASHJOIN,
	RCF_COUNT
} RobustCheckFunc;

static const char *const robust_check_func_names[RCF_COUNT] = {
	"cost_seqscan",
	"cost_index",
	"cost_gather",
	"cost_gather_merge",
	"nestloop",
	"mergejoin",
	"hashjoin"
};

/* What is compared */
typedef enum RobustCheckField
{
	RCX_ROWS,
	RCX_STARTUP,
	RCX_TOTAL,
	RCX_DECISION,				/* mergejoin materialize / mark-restore */
	RCX_LENGTH,					/* a vector of unexpected length */
	RCX_COUNT
} RobustCheckField;

static const char *const robust_check_field_names[RCX_COUNT] = {
	"rows",
	"startup_cost",
	"total_cost",
	"decision",
	"vector length"
};

typedef struct RobustCheckFuncStats
{
	int64		checked;		/* paths checked */
	int64		differing;		/* paths with at least one difference */
	int64		field_differing[RCX_COUNT]; /* paths differing per field */
	double		max_rel_diff;	/* largest relative cost/rows difference */
} RobustCheckFuncStats;

struct RobustCheckStats
{
	RobustCheckFuncStats funcs[RCF_COUNT];
	int64		skipped;		/* paths whose inputs could not be copied */

	/* paths given constant vectors, by plan node type */
	int			nfill_types;
	NodeTag		fill_types[ROBUST_CHECK_MAX_FILL_TYPES];
	int64		fill_counts[ROBUST_CHECK_MAX_FILL_TYPES];
	int64		fill_other;		/* types beyond the array */

	/* pass-2 paths compared with the pass-1 envelope (robust_check_envelope) */
	int64		env_paths;		/* paths compared */
	int64		env_samples;	/* (path, sample) pairs compared */
	int64		env_below;		/* pairs cheaper than the envelope */
	int64		env_below_fuzz; /* pairs cheaper by more than 1% */
	int64		env_paths_below;	/* paths with at least one such pair */
};

/* Differences found for one field of one path */
typedef struct FieldResult
{
	int			ndiff;			/* samples that differ */
	int			first;			/* first differing sample, or -1 */
	double		first_2p;
	double		first_1p;
	double		max_rel;		/* largest relative difference */
} FieldResult;

/* State of the check of one path */
typedef struct PathCheck
{
	PlannerInfo *root;
	RobustCheckFunc func;
	const Path *path;
	int			nsamples;
	int			save_pass;
	int			save_round;
	MemoryContext oldcxt;
	MemoryContext cxt;
	FieldResult fields[RCX_COUNT];
} PathCheck;


/*
 * robust_check_begin
 *		Set up the counters for a query level, if the check is enabled.
 *
 * Called by subquery_planner right after it creates the PlannerInfo.  Roots
 * built elsewhere (plan_cluster_use_sort, ...) have no counters; their
 * checks still write LOG lines.  The minmax-aggregate subroots are copies of
 * their parent and share its counters.
 */
void
robust_check_begin(PlannerInfo *root)
{
	/*
	 * A check that failed with an error inside a 1p function may have left
	 * a frozen decision behind; planning starts without one.
	 */
	robust_mergejoin_frozen_materialize = -1;

	if (enable_rows_dist && robust_debug_check_costs)
		root->robust_check = palloc0(sizeof(RobustCheckStats));
	else
		root->robust_check = NULL;
}

/*
 * robust_check_note_fill
 *		Count a path that got constant vectors from robust_fill_path_samples.
 *
 * Such paths have no 2p cost function, so their uncertainty is ignored.
 * The counts measure how much of the plan space the vector model covers.
 */
void
robust_check_note_fill(PlannerInfo *root, const Path *path)
{
	RobustCheckStats *stats;
	int			i;

	if (root == NULL || root->robust_check == NULL)
		return;

	/*
	 * Only passes 0 and 2 search with the vectors.  In pass 1 the 1P
	 * branches also attach constant vectors to join and gather inputs, and
	 * upper planning (pass 3) attaches them to the paths it adds, but
	 * nothing reads them there, so they say nothing about the coverage of
	 * the vector model.
	 */
	if (root->pass != 0 && root->pass != 2)
		return;

	/*
	 * A query without joins never reaches the join search, so its upper
	 * planning still runs with pass 0; upper relations are not part of the
	 * vector model either way.
	 */
	if (path->parent == NULL || IS_UPPER_REL(path->parent))
		return;
	stats = root->robust_check;

	for (i = 0; i < stats->nfill_types; i++)
	{
		if (stats->fill_types[i] == path->pathtype)
		{
			stats->fill_counts[i]++;
			return;
		}
	}
	if (stats->nfill_types < ROBUST_CHECK_MAX_FILL_TYPES)
	{
		stats->fill_types[stats->nfill_types] = path->pathtype;
		stats->fill_counts[stats->nfill_types] = 1;
		stats->nfill_types++;
	}
	else
		stats->fill_other++;
}

/*
 * plan_type_name
 *		Name of a path's plan node type, as EXPLAIN would show it.
 */
static const char *
plan_type_name(NodeTag pathtype)
{
	switch (pathtype)
	{
		case T_SeqScan:
			return "SeqScan";
		case T_SampleScan:
			return "SampleScan";
		case T_IndexScan:
			return "IndexScan";
		case T_IndexOnlyScan:
			return "IndexOnlyScan";
		case T_BitmapIndexScan:
			return "BitmapIndexScan";
		case T_BitmapHeapScan:
			return "BitmapHeapScan";
		case T_BitmapAnd:
			return "BitmapAnd";
		case T_BitmapOr:
			return "BitmapOr";
		case T_TidScan:
			return "TidScan";
		case T_TidRangeScan:
			return "TidRangeScan";
		case T_SubqueryScan:
			return "SubqueryScan";
		case T_FunctionScan:
			return "FunctionScan";
		case T_TableFuncScan:
			return "TableFuncScan";
		case T_ValuesScan:
			return "ValuesScan";
		case T_CteScan:
			return "CteScan";
		case T_NamedTuplestoreScan:
			return "NamedTuplestoreScan";
		case T_WorkTableScan:
			return "WorkTableScan";
		case T_ForeignScan:
			return "ForeignScan";
		case T_CustomScan:
			return "CustomScan";
		case T_Result:
			return "Result";
		case T_ProjectSet:
			return "ProjectSet";
		case T_Append:
			return "Append";
		case T_MergeAppend:
			return "MergeAppend";
		case T_RecursiveUnion:
			return "RecursiveUnion";
		case T_Material:
			return "Material";
		case T_Memoize:
			return "Memoize";
		case T_Sort:
			return "Sort";
		case T_IncrementalSort:
			return "IncrementalSort";
		case T_Group:
			return "Group";
		case T_Agg:
			return "Agg";
		case T_WindowAgg:
			return "WindowAgg";
		case T_Unique:
			return "Unique";
		case T_Gather:
			return "Gather";
		case T_GatherMerge:
			return "GatherMerge";
		case T_SetOp:
			return "SetOp";
		case T_LockRows:
			return "LockRows";
		case T_Limit:
			return "Limit";
		case T_NestLoop:
			return "NestLoop";
		case T_MergeJoin:
			return "MergeJoin";
		case T_HashJoin:
			return "HashJoin";
		case T_ModifyTable:
			return "ModifyTable";
		default:
			return psprintf("node type %d", (int) pathtype);
	}
}

/*
 * path_node_size
 *		Size of a path node, so that it can be copied with memcpy.
 *
 * Path nodes have no copyObject support.  Returns 0 for node types we
 * cannot size (CustomPath may be extended by its provider).
 */
static Size
path_node_size(const Path *path)
{
	switch (nodeTag(path))
	{
		case T_Path:
			return sizeof(Path);
		case T_IndexPath:
			return sizeof(IndexPath);
		case T_BitmapHeapPath:
			return sizeof(BitmapHeapPath);
		case T_BitmapAndPath:
			return sizeof(BitmapAndPath);
		case T_BitmapOrPath:
			return sizeof(BitmapOrPath);
		case T_TidPath:
			return sizeof(TidPath);
		case T_TidRangePath:
			return sizeof(TidRangePath);
		case T_SubqueryScanPath:
			return sizeof(SubqueryScanPath);
		case T_ForeignPath:
			return sizeof(ForeignPath);
		case T_AppendPath:
			return sizeof(AppendPath);
		case T_MergeAppendPath:
			return sizeof(MergeAppendPath);
		case T_GroupResultPath:
			return sizeof(GroupResultPath);
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
		case T_NestPath:
			return sizeof(NestPath);
		case T_MergePath:
			return sizeof(MergePath);
		case T_HashPath:
			return sizeof(HashPath);
		case T_ProjectionPath:
			return sizeof(ProjectionPath);
		case T_ProjectSetPath:
			return sizeof(ProjectSetPath);
		case T_SortPath:
			return sizeof(SortPath);
		case T_IncrementalSortPath:
			return sizeof(IncrementalSortPath);
		case T_GroupPath:
			return sizeof(GroupPath);
		case T_UpperUniquePath:
			return sizeof(UpperUniquePath);
		case T_AggPath:
			return sizeof(AggPath);
		case T_GroupingSetsPath:
			return sizeof(GroupingSetsPath);
		case T_MinMaxAggPath:
			return sizeof(MinMaxAggPath);
		case T_WindowAggPath:
			return sizeof(WindowAggPath);
		case T_SetOpPath:
			return sizeof(SetOpPath);
		case T_RecursiveUnionPath:
			return sizeof(RecursiveUnionPath);
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

/* Length of a vector; 0 for none */
static int
vec_len(const Sample *v)
{
	return (v == NULL) ? 0 : v->sample_count;
}

/*
 * sample_at
 *		Component s of a vector, with the scalar as fallback.
 *
 * A length-1 vector is a constant.  A vector shorter than the sample count
 * is reported as a length difference by the caller; here we just stay in
 * bounds.
 */
static double
sample_at(const Sample *v, int s, double scalar)
{
	if (v == NULL || v->sample_count <= 0)
		return scalar;
	if (v->sample_count == 1)
		return v->sample[0];
	return v->sample[Min(s, v->sample_count - 1)];
}

/*
 * project_path
 *		Scratch copy of an input path with its scalars set to sample s.
 *
 * Returns NULL if the node cannot be copied.
 */
static Path *
project_path(const Path *path, int s)
{
	Size		size = path_node_size(path);
	Path	   *copy;

	if (size == 0)
		return NULL;
	copy = palloc(size);
	memcpy(copy, path, size);
	copy->rows = sample_at(path->rows_sample, s, path->rows);
	copy->startup_cost = sample_at(path->startup_cost_sample, s,
								   path->startup_cost);
	copy->total_cost = sample_at(path->total_cost_sample, s,
								 path->total_cost);

	/*
	 * The fields below a node that cost_rescan reads (joincost_ext.c's
	 * cost_rescan_at projects the same ones): the batches of a hash join,
	 * and the subpath and number of calls of a Memoize.
	 */
	if (IsA(copy, HashPath))
	{
		HashPath   *hcopy = (HashPath *) copy;

		hcopy->num_batches = (int) sample_at(hcopy->num_batches_sample, s,
											 hcopy->num_batches);
	}
	else if (IsA(copy, MemoizePath))
	{
		MemoizePath *mcopy = (MemoizePath *) copy;
		Path	   *sub_s = project_path(mcopy->subpath, s);

		if (sub_s == NULL)
			return NULL;
		mcopy->subpath = sub_s;
		mcopy->calls = sample_at(mcopy->calls_sample, s, mcopy->calls);
	}
	return copy;
}

/* Scratch copy of a relation with rows set to sample s */
static RelOptInfo *
project_rel(const RelOptInfo *rel, int s)
{
	RelOptInfo *copy;

	if (rel == NULL)
		return NULL;
	copy = palloc(sizeof(RelOptInfo));
	memcpy(copy, rel, sizeof(RelOptInfo));
	copy->rows = sample_at(rel->rows_sample, s, rel->rows);
	return copy;
}

/* Scratch copy of a ParamPathInfo with ppi_rows set to sample s */
static ParamPathInfo *
project_ppi(const ParamPathInfo *ppi, int s)
{
	ParamPathInfo *copy;

	if (ppi == NULL)
		return NULL;
	copy = palloc(sizeof(ParamPathInfo));
	memcpy(copy, ppi, sizeof(ParamPathInfo));
	copy->ppi_rows = sample_at(ppi->ppi_rows_sample, s, ppi->ppi_rows);
	return copy;
}

/*
 * describe_relids
 *		"{a b c}" from the aliases of the base relations in relids.
 */
static void
describe_relids(StringInfo buf, PlannerInfo *root, Relids relids)
{
	int			relid = -1;
	bool		first = true;

	appendStringInfoChar(buf, '{');
	while ((relid = bms_next_member(relids, relid)) >= 0)
	{
		RangeTblEntry *rte;

		if (relid >= root->simple_rel_array_size)
			continue;
		rte = root->simple_rte_array[relid];
		if (rte == NULL || rte->rtekind == RTE_JOIN)
			continue;			/* outer-join relids add nothing here */
		if (!first)
			appendStringInfoChar(buf, ' ');
		appendStringInfoString(buf, rte->eref->aliasname);
		first = false;
	}
	appendStringInfoChar(buf, '}');
}

/* "HashJoin on {a b}, parameterized by {c}, partial" */
static char *
describe_path(PlannerInfo *root, const Path *path)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "%s on ", plan_type_name(path->pathtype));
	if (path->parent)
		describe_relids(&buf, root, path->parent->relids);
	else
		appendStringInfoString(&buf, "{}");
	if (path->param_info)
	{
		appendStringInfoString(&buf, ", parameterized by ");
		describe_relids(&buf, root, path->param_info->ppi_req_outer);
	}
	if (path->parallel_workers > 0)
		appendStringInfo(&buf, ", partial (%d workers)",
						 path->parallel_workers);
	return buf.data;
}

/*
 * check_start
 *		Set up the check of one path.
 *
 * "lens" holds the lengths of every vector involved (outputs and inputs);
 * the number of samples checked is the longest of them, or 1 if all are
 * constants.  Any other length is a length difference.
 */
static void
check_start(PathCheck *pc, PlannerInfo *root, RobustCheckFunc func,
			const Path *path, const int *lens, int nlens)
{
	int			i;

	memset(pc, 0, sizeof(PathCheck));
	pc->root = root;
	pc->func = func;
	pc->path = path;
	for (i = 0; i < RCX_COUNT; i++)
		pc->fields[i].first = -1;

	pc->nsamples = 1;
	for (i = 0; i < nlens; i++)
		pc->nsamples = Max(pc->nsamples, lens[i]);
	for (i = 0; i < nlens; i++)
	{
		if (lens[i] > 1 && lens[i] != pc->nsamples)
		{
			FieldResult *fr = &pc->fields[RCX_LENGTH];

			if (fr->ndiff++ == 0)
			{
				fr->first = 0;
				fr->first_2p = pc->nsamples;
				fr->first_1p = lens[i];
			}
		}
	}

	/* All scratch data goes into a context we drop at the end */
	pc->cxt = AllocSetContextCreate(CurrentMemoryContext,
									"robust cost check",
									ALLOCSET_DEFAULT_SIZES);
	pc->oldcxt = MemoryContextSwitchTo(pc->cxt);

	pc->save_pass = root->pass;
	pc->save_round = root->round;
}

/*
 * check_enter_sample / check_leave_sample
 *		Run the 1p functions in the environment of pass 1, round s.
 *
 * Any cost entry point the 1p function reaches then uses the 1p model too,
 * and none of them comes back into the checker.
 */
static void
check_enter_sample(PathCheck *pc, int s)
{
	pc->root->pass = 1;
	pc->root->round = s;
}

static void
check_leave_sample(PathCheck *pc)
{
	pc->root->pass = pc->save_pass;
	pc->root->round = pc->save_round;
}

/* Compare one value of sample s */
static void
check_value(PathCheck *pc, RobustCheckField field, int s,
			double v2p, double v1p)
{
	FieldResult *fr = &pc->fields[field];
	double		diff;
	double		scale;
	double		rel;

	if (v2p == v1p)
		return;
	diff = fabs(v2p - v1p);
	scale = Max(fabs(v2p), fabs(v1p));
	if (!isnan(diff) && diff <= ROBUST_CHECK_TOLERANCE * Max(scale, 1.0))
		return;

	rel = (isnan(diff) || scale == 0.0) ? get_float8_infinity() : diff / scale;
	if (fr->ndiff++ == 0)
	{
		fr->first = s;
		fr->first_2p = v2p;
		fr->first_1p = v1p;
	}
	if (rel > fr->max_rel || isnan(rel))
		fr->max_rel = rel;
}

/* Compare rows, startup_cost and total_cost of sample s */
static void
check_costs(PathCheck *pc, int s, const Path *scratch)
{
	const Path *path = pc->path;

	check_value(pc, RCX_ROWS, s,
				sample_at(path->rows_sample, s, path->rows),
				scratch->rows);
	check_value(pc, RCX_STARTUP, s,
				sample_at(path->startup_cost_sample, s, path->startup_cost),
				scratch->startup_cost);
	check_value(pc, RCX_TOTAL, s,
				sample_at(path->total_cost_sample, s, path->total_cost),
				scratch->total_cost);
}

/*
 * check_finish
 *		Report the differences of one path and drop the scratch data.
 */
static void
check_finish(PathCheck *pc)
{
	RobustCheckStats *stats = pc->root->robust_check;
	bool		differs = false;
	char	   *desc = NULL;
	int			i;

	check_leave_sample(pc);

	for (i = 0; i < RCX_COUNT; i++)
	{
		FieldResult *fr = &pc->fields[i];

		if (fr->ndiff == 0)
			continue;
		differs = true;
		if (desc == NULL)
			desc = describe_path(pc->root, pc->path);

		if (i == RCX_LENGTH)
			ereport(LOG,
					(errmsg_internal("robust cost check: %s, %s: vector of length %d where %d samples are checked",
									 robust_check_func_names[pc->func], desc,
									 (int) fr->first_1p, (int) fr->first_2p)));
		else if (i == RCX_DECISION)
			ereport(LOG,
					(errmsg_internal("robust cost check: %s, %s: decision differs in %d of %d samples (first at sample %d: 2p materialize_inner=%d skip_mark_restore=%d, 1p materialize_inner=%d skip_mark_restore=%d); costs of those samples not compared",
									 robust_check_func_names[pc->func], desc,
									 fr->ndiff, pc->nsamples, fr->first,
									 ((int) fr->first_2p) & 1,
									 (((int) fr->first_2p) >> 1) & 1,
									 ((int) fr->first_1p) & 1,
									 (((int) fr->first_1p) >> 1) & 1)));
		else
			ereport(LOG,
					(errmsg_internal("robust cost check: %s, %s: %s differs in %d of %d samples (first at sample %d: 2p %.10g, 1p %.10g; max relative difference %.3g)",
									 robust_check_func_names[pc->func], desc,
									 robust_check_field_names[i],
									 fr->ndiff, pc->nsamples, fr->first,
									 fr->first_2p, fr->first_1p,
									 fr->max_rel)));
	}

	if (stats)
	{
		RobustCheckFuncStats *fs = &stats->funcs[pc->func];

		fs->checked++;
		if (differs)
			fs->differing++;
		for (i = 0; i < RCX_COUNT; i++)
		{
			if (pc->fields[i].ndiff == 0)
				continue;
			fs->field_differing[i]++;
			if (i != RCX_DECISION && i != RCX_LENGTH)
				fs->max_rel_diff = Max(fs->max_rel_diff,
									   pc->fields[i].max_rel);
		}
	}

	MemoryContextSwitchTo(pc->oldcxt);
	MemoryContextDelete(pc->cxt);
}

/* A path whose inputs could not be copied; nothing was compared */
static void
check_skip(PathCheck *pc)
{
	RobustCheckStats *stats = pc->root->robust_check;

	check_leave_sample(pc);
	if (stats)
		stats->skipped++;
	MemoryContextSwitchTo(pc->oldcxt);
	MemoryContextDelete(pc->cxt);
}

/*
 * robust_check_report
 *		Report the counters of a query level and reset them.
 *
 * Called at the end of subquery_planner.
 */
void
robust_check_report(PlannerInfo *root)
{
	RobustCheckStats *stats = root->robust_check;
	StringInfoData detail;
	StringInfoData fills;
	int64		checked = 0;
	int64		differing = 0;
	int64		filled = 0;
	int			f;
	int			i;

	if (stats == NULL)
		return;

	initStringInfo(&detail);
	for (f = 0; f < RCF_COUNT; f++)
	{
		RobustCheckFuncStats *fs = &stats->funcs[f];

		checked += fs->checked;
		differing += fs->differing;
		if (fs->differing == 0)
			continue;
		if (detail.len > 0)
			appendStringInfoChar(&detail, '\n');
		appendStringInfo(&detail,
						 "%s: %lld of %lld paths differ (rows %lld, startup_cost %lld, total_cost %lld, decision %lld, vector length %lld; max relative difference %.3g).",
						 robust_check_func_names[f],
						 (long long) fs->differing, (long long) fs->checked,
						 (long long) fs->field_differing[RCX_ROWS],
						 (long long) fs->field_differing[RCX_STARTUP],
						 (long long) fs->field_differing[RCX_TOTAL],
						 (long long) fs->field_differing[RCX_DECISION],
						 (long long) fs->field_differing[RCX_LENGTH],
						 fs->max_rel_diff);
	}

	if (differing > 0)
		ereport(WARNING,
				(errmsg_internal("robust cost check: %lld of %lld paths differ between the 1p and 2p cost functions",
								 (long long) differing, (long long) checked),
				 errdetail_internal("%s", detail.data)));

	initStringInfo(&fills);
	for (i = 0; i < stats->nfill_types; i++)
	{
		filled += stats->fill_counts[i];
		appendStringInfo(&fills, "%s%s %lld", (i > 0) ? ", " : "",
						 plan_type_name(stats->fill_types[i]),
						 (long long) stats->fill_counts[i]);
	}
	if (stats->fill_other > 0)
	{
		filled += stats->fill_other;
		appendStringInfo(&fills, "%sother %lld", (i > 0) ? ", " : "",
						 (long long) stats->fill_other);
	}

	if (checked > 0 || filled > 0 || stats->skipped > 0)
		ereport(LOG,
				(errmsg_internal("robust cost check summary (query level %d): %lld paths checked, %lld differ, %lld skipped; constant vectors attached to %lld paths%s%s",
								 root->query_level,
								 (long long) checked, (long long) differing,
								 (long long) stats->skipped,
								 (long long) filled,
								 (filled > 0) ? ": " : "", fills.data)));

	if (stats->env_paths > 0)
		ereport(LOG,
				(errmsg_internal("robust envelope check (query level %d): %lld paths, %lld path samples compared; %lld paths and %lld path samples below the envelope, %lld of them by more than 1%%",
								 root->query_level,
								 (long long) stats->env_paths,
								 (long long) stats->env_samples,
								 (long long) stats->env_paths_below,
								 (long long) stats->env_below,
								 (long long) stats->env_below_fuzz)));

	pfree(detail.data);
	pfree(fills.data);
	memset(stats, 0, sizeof(RobustCheckStats));
}

/*
 * robust_check_envelope
 *		Compare the complete, unparameterized pass-2 paths of "rel" with its
 *		pass-1 minimum envelope (plan test T4).
 *
 * envelope[s] is C*(rel, s): the cheapest total cost PostgreSQL's own DP
 * found for this relation in pass-1 round s, i.e. with the cardinalities of
 * sample s.  A pass-2 path whose plan was also in that round's search space
 * costs at least that much at sample s.  A path that costs less means that
 * the envelope was computed from different inputs than the path (round view
 * missing, wrong relation matched, partial or parameterized costs mixed in),
 * or that PostgreSQL's DP never kept that plan: add_path discards a path
 * that is within the 1% fuzz factor of another, and some join methods only
 * use the cheapest input paths, while pass 2 keeps every candidate.  The
 * second kind cannot be told apart here, so the pairs below the envelope by
 * more than 1% are counted separately.
 *
 * Called by robust_join_search for every joinrel of pass 2, after the
 * level's paths are built and before they are pruned.
 */
void
robust_check_envelope(PlannerInfo *root, const RelOptInfo *rel,
					  const double *envelope)
{
	RobustCheckStats *stats = root->robust_check;
	ListCell   *lc;

	if (stats == NULL || envelope == NULL)
		return;

	foreach(lc, rel->pathlist)
	{
		const Path *path = (const Path *) lfirst(lc);
		const Sample *total = path->total_cost_sample;
		bool		below = false;

		if (path->param_info != NULL || total == NULL)
			continue;
		stats->env_paths++;
		for (int s = 0; s < error_sample_count; s++)
		{
			double		cost = (total->sample_count == 1) ?
				total->sample[0] : total->sample[s];

			stats->env_samples++;
			if (envelope[s] > cost * (1.0 + ROBUST_CHECK_TOLERANCE))
			{
				stats->env_below++;
				below = true;
				if (envelope[s] > cost * 1.01)
					stats->env_below_fuzz++;
			}
		}
		if (below)
			stats->env_paths_below++;
	}
}

/*
 * robust_check_seqscan
 *		Check cost_seqscan_2p against cost_seqscan_1p.
 */
void
robust_check_seqscan(PlannerInfo *root, const Path *path,
					 const RelOptInfo *baserel,
					 const ParamPathInfo *param_info)
{
	PathCheck	pc;
	int			lens[5];
	int			s;

	lens[0] = vec_len(path->rows_sample);
	lens[1] = vec_len(path->startup_cost_sample);
	lens[2] = vec_len(path->total_cost_sample);
	lens[3] = vec_len(baserel->rows_sample);
	lens[4] = param_info ? vec_len(param_info->ppi_rows_sample) : 0;
	check_start(&pc, root, RCF_SEQSCAN, path, lens, lengthof(lens));

	for (s = 0; s < pc.nsamples; s++)
	{
		Path	   *scratch = project_path(path, s);
		RelOptInfo *rel_s = project_rel(baserel, s);
		ParamPathInfo *ppi_s = project_ppi(param_info, s);

		scratch->parent = rel_s;
		scratch->param_info = ppi_s;

		check_enter_sample(&pc, s);
		cost_seqscan_1p(scratch, root, rel_s, ppi_s);
		check_leave_sample(&pc);

		check_costs(&pc, s, scratch);
	}
	check_finish(&pc);
}

/*
 * robust_check_index
 *		Check cost_index_2p against cost_index_1p.
 *
 * Both functions take the relation from path->indexinfo->rel, so the index
 * is copied too and pointed at the projected relation.
 */
void
robust_check_index(PlannerInfo *root, const IndexPath *path,
				   double loop_count, bool partial_path)
{
	const RelOptInfo *baserel = path->indexinfo->rel;
	const ParamPathInfo *param_info = path->path.param_info;
	PathCheck	pc;
	int			lens[5];
	int			s;

	/*
	 * A partial path for which no workers can be used is left half-costed by
	 * both functions and discarded by the caller.
	 */
	if (partial_path && path->path.parallel_workers <= 0)
		return;

	lens[0] = vec_len(path->path.rows_sample);
	lens[1] = vec_len(path->path.startup_cost_sample);
	lens[2] = vec_len(path->path.total_cost_sample);
	lens[3] = vec_len(baserel->rows_sample);
	lens[4] = param_info ? vec_len(param_info->ppi_rows_sample) : 0;
	check_start(&pc, root, RCF_INDEX, &path->path, lens, lengthof(lens));

	for (s = 0; s < pc.nsamples; s++)
	{
		IndexPath  *scratch = (IndexPath *) project_path(&path->path, s);
		RelOptInfo *rel_s = project_rel(baserel, s);
		IndexOptInfo *index_s = palloc(sizeof(IndexOptInfo));

		memcpy(index_s, path->indexinfo, sizeof(IndexOptInfo));
		index_s->rel = rel_s;
		scratch->indexinfo = index_s;
		scratch->path.parent = rel_s;
		scratch->path.param_info = project_ppi(param_info, s);

		check_enter_sample(&pc, s);
		cost_index_1p(scratch, root, loop_count, partial_path);
		check_leave_sample(&pc);

		check_costs(&pc, s, &scratch->path);
	}
	check_finish(&pc);
}

/*
 * project_rows_arg
 *		Projection of a caller-supplied row estimate (see robust_check_gather).
 */
static double
project_rows_arg(double rows, const Path *subpath, const Path *sub_s)
{
	if (subpath->rows <= 0)
		return rows;
	return rows * (sub_s->rows / subpath->rows);
}

/*
 * robust_check_gather
 *		Check cost_gather_2p against cost_gather_1p.
 *
 * When the caller passes "rows" it is an estimate derived from a partial
 * path of the same relation (subpath->rows * subpath->parallel_workers in
 * generate_gather_paths, the cheapest partial path's in upper planning), so
 * it is projected proportionally: rows * (subpath rows at s / subpath rows).
 */
void
robust_check_gather(PlannerInfo *root, const GatherPath *path,
					const RelOptInfo *rel, const ParamPathInfo *param_info,
					const double *rows)
{
	const Path *subpath = path->subpath;
	PathCheck	pc;
	int			lens[8];
	int			s;

	lens[0] = vec_len(path->path.rows_sample);
	lens[1] = vec_len(path->path.startup_cost_sample);
	lens[2] = vec_len(path->path.total_cost_sample);
	lens[3] = vec_len(subpath->rows_sample);
	lens[4] = vec_len(subpath->startup_cost_sample);
	lens[5] = vec_len(subpath->total_cost_sample);
	lens[6] = (rel && !rows) ? vec_len(rel->rows_sample) : 0;
	lens[7] = (param_info && !rows) ? vec_len(param_info->ppi_rows_sample) : 0;
	check_start(&pc, root, RCF_GATHER, &path->path, lens, lengthof(lens));

	for (s = 0; s < pc.nsamples; s++)
	{
		GatherPath *scratch = (GatherPath *) project_path(&path->path, s);
		Path	   *sub_s = project_path(subpath, s);
		RelOptInfo *rel_s = project_rel(rel, s);
		ParamPathInfo *ppi_s = project_ppi(param_info, s);
		double		rows_s = 0;

		if (sub_s == NULL)
		{
			check_skip(&pc);
			return;
		}
		scratch->subpath = sub_s;
		scratch->path.parent = rel_s;
		scratch->path.param_info = ppi_s;
		if (rows)
			rows_s = project_rows_arg(*rows, subpath, sub_s);

		check_enter_sample(&pc, s);
		cost_gather_1p(scratch, root, rel_s, ppi_s, rows ? &rows_s : NULL);
		check_leave_sample(&pc);

		check_costs(&pc, s, &scratch->path);
	}
	check_finish(&pc);
}

/*
 * robust_check_gather_merge
 *		Check cost_gather_merge_2p against cost_gather_merge_1p.
 *
 * The input costs are recomputed from the projected subpath the way
 * create_gather_merge_path computes them: the subpath's own costs when it is
 * already sorted, else those of a Sort over it.  "rows" is projected as in
 * robust_check_gather.
 */
void
robust_check_gather_merge(PlannerInfo *root, const GatherMergePath *path,
						  const RelOptInfo *rel,
						  const ParamPathInfo *param_info,
						  const double *rows)
{
	const Path *subpath = path->subpath;
	PathCheck	pc;
	int			lens[8];
	int			s;

	lens[0] = vec_len(path->path.rows_sample);
	lens[1] = vec_len(path->path.startup_cost_sample);
	lens[2] = vec_len(path->path.total_cost_sample);
	lens[3] = vec_len(subpath->rows_sample);
	lens[4] = vec_len(subpath->startup_cost_sample);
	lens[5] = vec_len(subpath->total_cost_sample);
	lens[6] = (rel && !rows) ? vec_len(rel->rows_sample) : 0;
	lens[7] = (param_info && !rows) ? vec_len(param_info->ppi_rows_sample) : 0;
	check_start(&pc, root, RCF_GATHER_MERGE, &path->path, lens,
				lengthof(lens));

	for (s = 0; s < pc.nsamples; s++)
	{
		GatherMergePath *scratch;
		Path	   *sub_s = project_path(subpath, s);
		RelOptInfo *rel_s = project_rel(rel, s);
		ParamPathInfo *ppi_s = project_ppi(param_info, s);
		Cost		input_startup_cost;
		Cost		input_total_cost;
		double		rows_s = 0;

		if (sub_s == NULL)
		{
			check_skip(&pc);
			return;
		}
		scratch = (GatherMergePath *) project_path(&path->path, s);
		scratch->subpath = sub_s;
		scratch->path.parent = rel_s;
		scratch->path.param_info = ppi_s;
		if (rows)
			rows_s = project_rows_arg(*rows, subpath, sub_s);

		check_enter_sample(&pc, s);
		if (pathkeys_contained_in(path->path.pathkeys, sub_s->pathkeys))
		{
			input_startup_cost = sub_s->startup_cost;
			input_total_cost = sub_s->total_cost;
		}
		else
		{
			Path		sort_path;	/* dummy for result of cost_sort */

			cost_sort(&sort_path, root, path->path.pathkeys,
					  sub_s->total_cost, sub_s->rows,
					  sub_s->pathtarget->width, 0.0, work_mem, -1);
			input_startup_cost = sort_path.startup_cost;
			input_total_cost = sort_path.total_cost;
		}
		cost_gather_merge_1p(scratch, root, rel_s, ppi_s,
							 input_startup_cost, input_total_cost,
							 rows ? &rows_s : NULL);
		check_leave_sample(&pc);

		check_costs(&pc, s, &scratch->path);
	}
	check_finish(&pc);
}

/*
 * join_lens
 *		Vector lengths involved in checking a join path.
 */
static void
join_lens(const JoinPath *jpath, int *lens)
{
	const Path *path = &jpath->path;
	const Path *outer = jpath->outerjoinpath;
	const Path *inner = jpath->innerjoinpath;

	lens[0] = vec_len(path->rows_sample);
	lens[1] = vec_len(path->startup_cost_sample);
	lens[2] = vec_len(path->total_cost_sample);
	lens[3] = vec_len(outer->rows_sample);
	lens[4] = vec_len(outer->startup_cost_sample);
	lens[5] = vec_len(outer->total_cost_sample);
	lens[6] = vec_len(inner->rows_sample);
	lens[7] = vec_len(inner->startup_cost_sample);
	lens[8] = vec_len(inner->total_cost_sample);
	lens[9] = path->parent ? vec_len(path->parent->rows_sample) : 0;
	lens[10] = path->param_info ?
		vec_len(path->param_info->ppi_rows_sample) : 0;
}

#define JOIN_NLENS 11

/*
 * project_join
 *		Point a scratch join path at projected inputs, relation and ppi.
 *
 * Returns false if an input cannot be copied.
 */
static bool
project_join(JoinPath *scratch, const JoinPath *jpath, int s)
{
	Path	   *outer_s = project_path(jpath->outerjoinpath, s);
	Path	   *inner_s = project_path(jpath->innerjoinpath, s);

	if (outer_s == NULL || inner_s == NULL)
		return false;
	scratch->outerjoinpath = outer_s;
	scratch->innerjoinpath = inner_s;
	scratch->path.parent = project_rel(jpath->path.parent, s);
	scratch->path.param_info = project_ppi(jpath->path.param_info, s);
	return true;
}

/*
 * robust_check_nestloop
 *		Check {initial,final}_cost_nestloop_2p against the 1p pair.
 *
 * try_nestloop_path costs the initial phase with the inner path it was
 * given, and only then may replace the inner by a copy reparameterized by
 * the child outer rel (reparameterize_path_by_child, partitionwise joins).
 * PostgreSQL's cost is the initial phase of the original inner plus the
 * final phase of the replacement (a Memoize over the copy estimates its
 * distinct keys from the child's statistics), so the 1p initial phase is run
 * on the inner the 2p initial phase saw (workspace->nestloop_initial_inner)
 * and the final phase on the path's own inner.
 */
void
robust_check_nestloop(PlannerInfo *root, const NestPath *path,
					  const JoinCostWorkspace *workspace_2p,
					  JoinPathExtraData *extra)
{
	const JoinPath *jpath = &path->jpath;
	const Path *initial_inner = workspace_2p->nestloop_initial_inner ?
		workspace_2p->nestloop_initial_inner : jpath->innerjoinpath;
	List	   *saved_rows = NIL;
	PathCheck	pc;
	int			lens[JOIN_NLENS];
	int			s;

	join_lens(jpath, lens);
	check_start(&pc, root, RCF_NESTLOOP, &jpath->path, lens, JOIN_NLENS);

	for (s = 0; s < pc.nsamples; s++)
	{
		NestPath   *scratch = (NestPath *) project_path(&jpath->path, s);
		Path	   *initial_inner_s;
		JoinCostWorkspace workspace;

		if (!project_join(&scratch->jpath, jpath, s))
		{
			check_skip(&pc);
			return;
		}
		if (initial_inner == jpath->innerjoinpath)
			initial_inner_s = scratch->jpath.innerjoinpath;
		else if ((initial_inner_s = project_path(initial_inner, s)) == NULL)
		{
			check_skip(&pc);
			return;
		}
		memset(&workspace, 0, sizeof(workspace));

		check_enter_sample(&pc, s);
		/* a Memoize inner reads its keys' base relations (see cost_ext) */
		if (IsA(initial_inner_s, MemoizePath))
			saved_rows = robust_project_base_rows(root,
												  pull_varnos(root, (Node *) ((MemoizePath *) initial_inner_s)->param_exprs),
												  s);
		initial_cost_nestloop_1p(root, &workspace, scratch->jpath.jointype,
								 scratch->jpath.outerjoinpath,
								 initial_inner_s, extra);
		robust_restore_base_rows(saved_rows);
		saved_rows = NIL;
		final_cost_nestloop_1p(root, scratch, &workspace, extra);
		check_leave_sample(&pc);

		check_costs(&pc, s, &scratch->jpath.path);
	}
	check_finish(&pc);
}

/*
 * robust_check_mergejoin
 *		Check {initial,final}_cost_mergejoin_2p against the 1p pair.
 *
 * materialize_inner is a structural decision that the 2p function makes
 * once for all samples; the 1p function is run with that decision frozen
 * (robust_mergejoin_frozen_materialize) and the costs compared.
 * skip_mark_restore does not depend on the inputs; a difference there is
 * reported as a decision difference.
 */
void
robust_check_mergejoin(PlannerInfo *root, const MergePath *path,
					   JoinPathExtraData *extra)
{
	const JoinPath *jpath = &path->jpath;
	int			decision_2p;
	PathCheck	pc;
	int			lens[JOIN_NLENS];
	int			s;

	decision_2p = (path->materialize_inner ? 1 : 0) |
		(path->skip_mark_restore ? 2 : 0);

	join_lens(jpath, lens);
	check_start(&pc, root, RCF_MERGEJOIN, &jpath->path, lens, JOIN_NLENS);

	for (s = 0; s < pc.nsamples; s++)
	{
		MergePath  *scratch = (MergePath *) project_path(&jpath->path, s);
		JoinCostWorkspace workspace;
		int			decision_1p;

		if (!project_join(&scratch->jpath, jpath, s))
		{
			check_skip(&pc);
			return;
		}
		memset(&workspace, 0, sizeof(workspace));

		check_enter_sample(&pc, s);
		initial_cost_mergejoin_1p(root, &workspace, scratch->jpath.jointype,
								  scratch->path_mergeclauses,
								  scratch->jpath.outerjoinpath,
								  scratch->jpath.innerjoinpath,
								  scratch->outersortkeys,
								  scratch->innersortkeys, extra);
		robust_mergejoin_frozen_materialize = path->materialize_inner ? 1 : 0;
		final_cost_mergejoin_1p(root, scratch, &workspace, extra);
		robust_mergejoin_frozen_materialize = -1;
		check_leave_sample(&pc);

		decision_1p = (scratch->materialize_inner ? 1 : 0) |
			(scratch->skip_mark_restore ? 2 : 0);
		check_value(&pc, RCX_DECISION, s, decision_2p, decision_1p);
		check_costs(&pc, s, &scratch->jpath.path);
	}
	check_finish(&pc);
}

/*
 * robust_check_hashjoin
 *		Check {initial,final}_cost_hashjoin_2p against the 1p pair.
 *
 * A HashPath is parallel-aware exactly when it was costed with
 * parallel_hash (create_hashjoin_path is only asked for a parallel hash on
 * a joinrel that allows parallelism).
 */
void
robust_check_hashjoin(PlannerInfo *root, const HashPath *path,
					  JoinPathExtraData *extra)
{
	const JoinPath *jpath = &path->jpath;
	PathCheck	pc;
	int			lens[JOIN_NLENS];
	int			s;

	join_lens(jpath, lens);
	check_start(&pc, root, RCF_HASHJOIN, &jpath->path, lens, JOIN_NLENS);

	for (s = 0; s < pc.nsamples; s++)
	{
		HashPath   *scratch = (HashPath *) project_path(&jpath->path, s);
		JoinCostWorkspace workspace;

		if (!project_join(&scratch->jpath, jpath, s))
		{
			check_skip(&pc);
			return;
		}
		memset(&workspace, 0, sizeof(workspace));

		check_enter_sample(&pc, s);
		initial_cost_hashjoin_1p(root, &workspace, scratch->jpath.jointype,
								 scratch->path_hashclauses,
								 scratch->jpath.outerjoinpath,
								 scratch->jpath.innerjoinpath, extra,
								 scratch->jpath.path.parallel_aware);
		final_cost_hashjoin_1p(root, scratch, &workspace, extra);
		check_leave_sample(&pc);

		check_costs(&pc, s, &scratch->jpath.path);
	}
	check_finish(&pc);
}
