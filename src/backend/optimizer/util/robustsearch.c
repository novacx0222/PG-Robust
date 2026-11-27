/*-------------------------------------------------------------------------
 *
 * robustsearch.c
 *	  The join search of RobDP: envelope rounds and their bookkeeping.
 *
 * See optimizer/robustsearch.h.  robust_join_search() in allpaths.c drives
 * the passes; this file projects the level-1 relations onto a round's
 * scenario and puts them back, saves and restores the estimates cached in
 * RestrictInfos, records the envelope and writes the debug events.
 *
 * src/backend/optimizer/util/robustsearch.c
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
#include "optimizer/robustsearch.h"
#include "optimizer/robustselect.h"
#include "utils/hsearch.h"

/* The estimates a RestrictInfo caches, which a round may change */
typedef struct SavedRinfo
{
	RestrictInfo *rinfo;		/* hash key */
	Selectivity norm_selec;
	Selectivity outer_selec;
	Selectivity left_bucketsize;
	Selectivity right_bucketsize;
	Selectivity left_mcvfreq;
	Selectivity right_mcvfreq;
} SavedRinfo;

/* C*(U, s) of one relation U */
typedef struct EnvEntry
{
	Relids		relids;			/* hash key */
	int			level;
	bool		child;			/* a child join of a partitionwise join */
	double	   *cost;			/* complete paths, per scenario; NaN where
								 * not built */
	double	   *partial;		/* partial paths, likewise; NaN where the
								 * relation has none */
} EnvEntry;

/* What a round view overwrote */
typedef struct RelSave
{
	RelOptInfo *rel;
	double		rows;
	List	   *pathlist;
	List	   *partial_pathlist;
	Path	   *cheapest_startup_path;
	Path	   *cheapest_total_path;
	Path	   *cheapest_unique_path;
	List	   *cheapest_parameterized_paths;
	List	   *ppilist;
	int			nppi;
	RelOptInfo **part_rels;		/* copy of the array, or NULL */
} RelSave;

typedef struct PathSave
{
	Path	   *path;
	double		rows;
	Cost		startup;
	Cost		total;
	int			nbatches;
} PathSave;

typedef struct PpiSave
{
	ParamPathInfo *ppi;
	double		rows;
} PpiSave;

struct RobustSearch
{
	PlannerInfo *root;
	MemoryContext cxt;			/* the search's memory */
	List	   *initial_rels;
	List	   *view_rels;		/* the relations a round view projects */
	HTAB	   *rinfos;			/* SavedRinfo */
	void	   *cost_save;		/* robustcost.c's state */
	HTAB	   *envelope;		/* EnvEntry */
	List	   *env_order;		/* EnvEntry, in the order first recorded */
	int			round;			/* current round, -1 outside */
	bool		select;			/* the main pass selects (robustselect.h) */
	/* the current round's view (allocated in the round's memory) */
	List	   *rel_saves;
	List	   *path_saves;
	List	   *ppi_saves;
};

/* ----------------------------------------------------- RestrictInfos */

static void
save_rinfo(RobustSearch *rs, RestrictInfo *rinfo)
{
	SavedRinfo *e;
	bool		found;

	e = (SavedRinfo *) hash_search(rs->rinfos, &rinfo, HASH_ENTER, &found);
	if (found)
		return;
	e->norm_selec = rinfo->norm_selec;
	e->outer_selec = rinfo->outer_selec;
	e->left_bucketsize = rinfo->left_bucketsize;
	e->right_bucketsize = rinfo->right_bucketsize;
	e->left_mcvfreq = rinfo->left_mcvfreq;
	e->right_mcvfreq = rinfo->right_mcvfreq;
}

static void
save_rinfo_list(RobustSearch *rs, List *rinfos)
{
	ListCell   *lc;

	foreach(lc, rinfos)
	{
		if (IsA(lfirst(lc), RestrictInfo))
			save_rinfo(rs, (RestrictInfo *) lfirst(lc));
	}
}

/*
 * The RestrictInfos whose caches a join search can fill: the restriction
 * and join clauses of the relations, and the clauses of the equivalence
 * classes, including those derived so far.  (Clauses derived later, during
 * a round, are allocated in the planner's memory and kept, see
 * create_join_clause; reset_rinfos clears their caches.)
 */
static void
snapshot_rinfos(RobustSearch *rs)
{
	PlannerInfo *root = rs->root;
	HASHCTL		ctl;
	ListCell   *lc;

	ctl.keysize = sizeof(RestrictInfo *);
	ctl.entrysize = sizeof(SavedRinfo);
	ctl.hcxt = CurrentMemoryContext;
	rs->rinfos = hash_create("RobDP RestrictInfo caches", 256, &ctl,
							 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	for (int i = 1; i < root->simple_rel_array_size; i++)
	{
		RelOptInfo *rel = root->simple_rel_array[i];

		if (rel == NULL)
			continue;
		save_rinfo_list(rs, rel->baserestrictinfo);
		save_rinfo_list(rs, rel->joininfo);
	}
	foreach(lc, root->join_rel_list)
		save_rinfo_list(rs, ((RelOptInfo *) lfirst(lc))->joininfo);
	foreach(lc, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);

		save_rinfo_list(rs, ec->ec_sources);
		save_rinfo_list(rs, ec->ec_derives);
	}
}

static void
clear_rinfo(RestrictInfo *rinfo)
{
	rinfo->norm_selec = -1;
	rinfo->outer_selec = -1;
	rinfo->left_bucketsize = -1;
	rinfo->right_bucketsize = -1;
	rinfo->left_mcvfreq = -1;
	rinfo->right_mcvfreq = -1;
}

/* Put the caches back as they were before the search */
static void
reset_rinfos(RobustSearch *rs)
{
	HASH_SEQ_STATUS hs;
	SavedRinfo *e;
	ListCell   *lc;

	hash_seq_init(&hs, rs->rinfos);
	while ((e = (SavedRinfo *) hash_seq_search(&hs)) != NULL)
	{
		RestrictInfo *rinfo = e->rinfo;

		rinfo->norm_selec = e->norm_selec;
		rinfo->outer_selec = e->outer_selec;
		rinfo->left_bucketsize = e->left_bucketsize;
		rinfo->right_bucketsize = e->right_bucketsize;
		rinfo->left_mcvfreq = e->left_mcvfreq;
		rinfo->right_mcvfreq = e->right_mcvfreq;
	}
	foreach(lc, rs->root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);
		ListCell   *lc2;

		foreach(lc2, ec->ec_derives)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc2);

			if (hash_search(rs->rinfos, &rinfo, HASH_FIND, NULL) == NULL)
				clear_rinfo(rinfo);
		}
	}
}

/* ------------------------------------------------------- round view */

static inline double
vec_at(const RobustVec *v, double scalar, int s)
{
	return v != NULL ? robust_vec_at(v, s) : scalar;
}

static void
view_path(RobustSearch *rs, Path *path, int s)
{
	RobustPathCost *rc = path->robust;
	PathSave   *ps;

	if (rc == NULL ||
		(rc->rows == NULL && rc->startup == NULL && rc->total == NULL &&
		 rc->nbatches == NULL))
		return;
	ps = palloc(sizeof(PathSave));
	ps->path = path;
	ps->rows = path->rows;
	ps->startup = path->startup_cost;
	ps->total = path->total_cost;
	ps->nbatches = IsA(path, HashPath) ? ((HashPath *) path)->num_batches : 0;
	rs->path_saves = lappend(rs->path_saves, ps);

	path->rows = vec_at(rc->rows, path->rows, s);
	path->startup_cost = vec_at(rc->startup, path->startup_cost, s);
	path->total_cost = vec_at(rc->total, path->total_cost, s);
	if (IsA(path, HashPath) && rc->nbatches != NULL)
		((HashPath *) path)->num_batches = rc->nbatches[s];
}

/*
 * Project a relation onto scenario s: its rows, the rows of its
 * ParamPathInfos, the costs of its paths, and so its cheapest paths.
 */
static void
view_rel(RobustSearch *rs, RelOptInfo *rel, int s)
{
	RelSave    *r = palloc(sizeof(RelSave));
	ListCell   *lc;

	r->rel = rel;
	r->rows = rel->rows;
	r->pathlist = rel->pathlist;
	r->partial_pathlist = rel->partial_pathlist;
	r->cheapest_startup_path = rel->cheapest_startup_path;
	r->cheapest_total_path = rel->cheapest_total_path;
	r->cheapest_unique_path = rel->cheapest_unique_path;
	r->cheapest_parameterized_paths = rel->cheapest_parameterized_paths;
	r->ppilist = rel->ppilist;
	r->nppi = list_length(rel->ppilist);
	r->part_rels = NULL;
	if (rel->part_rels != NULL && rel->nparts > 0)
	{
		r->part_rels = palloc(sizeof(RelOptInfo *) * rel->nparts);
		memcpy(r->part_rels, rel->part_rels, sizeof(RelOptInfo *) * rel->nparts);
	}
	rs->rel_saves = lappend(rs->rel_saves, r);

	rel->rows = vec_at(rel->robust_rows, rel->rows, s);
	foreach(lc, rel->ppilist)
	{
		ParamPathInfo *ppi = (ParamPathInfo *) lfirst(lc);
		PpiSave    *p;

		if (ppi->ppi_robust_rows == NULL)
			continue;
		p = palloc(sizeof(PpiSave));
		p->ppi = ppi;
		p->rows = ppi->ppi_rows;
		rs->ppi_saves = lappend(rs->ppi_saves, p);
		ppi->ppi_rows = robust_vec_at(ppi->ppi_robust_rows, s);
	}
	foreach(lc, rel->pathlist)
		view_path(rs, (Path *) lfirst(lc), s);
	foreach(lc, rel->partial_pathlist)
		view_path(rs, (Path *) lfirst(lc), s);

	/*
	 * The join builders take the first partial path for the cheapest, as
	 * add_partial_path keeps them in order of total cost: put them in the
	 * order of s (a stable sort, as add_partial_path inserts after equals).
	 */
	if (list_length(rel->partial_pathlist) > 1)
	{
		List	   *sorted = NIL;

		foreach(lc, rel->partial_pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);
			int			at = 0;
			ListCell   *lc2;

			foreach(lc2, sorted)
			{
				if (p->total_cost < ((Path *) lfirst(lc2))->total_cost)
					break;
				at++;
			}
			sorted = list_insert_nth(sorted, at, p);
		}
		rel->partial_pathlist = sorted;
	}

	/* set_cheapest also forgets the unique-ified path */
	if (rel->pathlist != NIL)
		set_cheapest(rel);
	rel->cheapest_unique_path = NULL;
}

static void
unview(RobustSearch *rs)
{
	ListCell   *lc;

	foreach(lc, rs->path_saves)
	{
		PathSave   *ps = (PathSave *) lfirst(lc);

		ps->path->rows = ps->rows;
		ps->path->startup_cost = ps->startup;
		ps->path->total_cost = ps->total;
		if (IsA(ps->path, HashPath))
			((HashPath *) ps->path)->num_batches = ps->nbatches;
	}
	foreach(lc, rs->ppi_saves)
	{
		PpiSave    *p = (PpiSave *) lfirst(lc);

		p->ppi->ppi_rows = p->rows;
	}
	foreach(lc, rs->rel_saves)
	{
		RelSave    *r = (RelSave *) lfirst(lc);
		RelOptInfo *rel = r->rel;

		rel->rows = r->rows;
		rel->pathlist = r->pathlist;
		rel->partial_pathlist = r->partial_pathlist;
		rel->cheapest_startup_path = r->cheapest_startup_path;
		rel->cheapest_total_path = r->cheapest_total_path;
		rel->cheapest_unique_path = r->cheapest_unique_path;
		rel->cheapest_parameterized_paths = r->cheapest_parameterized_paths;
		/* ParamPathInfos the round added were allocated in its memory */
		rel->ppilist = r->ppilist != NIL ?
			list_truncate(r->ppilist, r->nppi) : NIL;
		/* so were dummy partitions (build_dummy_partition_rel) */
		if (r->part_rels != NULL)
			memcpy(rel->part_rels, r->part_rels,
				   sizeof(RelOptInfo *) * rel->nparts);
	}
	rs->rel_saves = NIL;
	rs->path_saves = NIL;
	rs->ppi_saves = NIL;
}

/* ---------------------------------------------------------- envelope */

/*
 * C*(rel, s) of the round: the cheapest total cost of its complete paths
 * (its cheapest total path, Gather paths included), and of its partial
 * paths (per worker).
 */
static void
record(RobustSearch *rs, RelOptInfo *rel, int level, bool child)
{
	EnvEntry   *e;
	bool		found;
	MemoryContext old;
	ListCell   *lc;

	if (rel->cheapest_total_path == NULL)
		return;
	e = (EnvEntry *) hash_search(rs->envelope, &rel->relids, HASH_FIND, NULL);
	if (e == NULL)
	{
		Relids		key;

		old = MemoryContextSwitchTo(rs->cxt);
		key = bms_copy(rel->relids);
		e = (EnvEntry *) hash_search(rs->envelope, &key, HASH_ENTER, &found);
		e->relids = key;
		e->level = level;
		e->child = child;
		e->cost = palloc(sizeof(double) * error_sample_count);
		e->partial = palloc(sizeof(double) * error_sample_count);
		for (int s = 0; s < error_sample_count; s++)
		{
			e->cost[s] = NAN;
			e->partial[s] = NAN;
		}
		rs->env_order = lappend(rs->env_order, e);
		MemoryContextSwitchTo(old);
	}
	e->cost[rs->round] = rel->cheapest_total_path->total_cost;
	foreach(lc, rel->partial_pathlist)
	{
		Cost		c = ((Path *) lfirst(lc))->total_cost;

		if (isnan(e->partial[rs->round]) || c < e->partial[rs->round])
			e->partial[rs->round] = c;
	}
}

/* The envelope of a relation (its complete or partial paths), or NULL */
static const double *
envelope_of(RobustSearch *rs, RelOptInfo *rel, bool partial)
{
	EnvEntry   *e;

	e = (EnvEntry *) hash_search(rs->envelope, &rel->relids, HASH_FIND, NULL);
	if (e == NULL)
		return NULL;
	return partial ? e->partial : e->cost;
}

/*
 * robust_search_envelope
 *		The envelope of rel (C*(U, s) of its complete or of its partial
 *		paths) the selection will compare to, as robust_search_finish_rel
 *		passes it, or NULL: none recorded, or no search running.  NaN in
 *		the scenarios that lack one.
 */
const double *
robust_search_envelope(PlannerInfo *root, RelOptInfo *rel, bool partial)
{
	RobustSearch *rs = root->robust_search;

	if (rs == NULL || rs->envelope == NULL)
		return NULL;
	return envelope_of(rs, rel, partial);
}

static void
dump_envelope(RobustSearch *rs)
{
	const int  *counts = robust_cost_round_counts();
	RobustDump	d;
	ListCell   *lc;

	foreach(lc, rs->env_order)
	{
		EnvEntry   *e = (EnvEntry *) lfirst(lc);
		RobustVec  *v = robust_vec_make(error_sample_count);

		memcpy(v->v, e->cost, sizeof(double) * error_sample_count);
		robust_dump_begin(&d, "env");
		robust_dump_relids(&d, "rels", rs->root, e->relids);
		robust_dump_int(&d, "level", e->level);
		robust_dump_bool(&d, "child", e->child);
		robust_dump_vec(&d, "cost", v);
		memcpy(v->v, e->partial, sizeof(double) * error_sample_count);
		robust_dump_vec(&d, "partial", v);
		robust_dump_end(&d);
		pfree(v);
	}
	robust_dump_begin(&d, "envstat");
	robust_dump_int(&d, "rounds", error_sample_count);
	robust_dump_int(&d, "rels", list_length(rs->env_order));
	robust_dump_int(&d, "const", counts[ROBUST_COST_CONST]);
	robust_dump_int(&d, "replay", counts[ROBUST_COST_REPLAY]);
	robust_dump_int(&d, "mismatch", counts[ROBUST_COST_MISMATCH]);
	robust_dump_int(&d, "unsupported", counts[ROBUST_COST_UNSUPPORTED]);
	robust_dump_int(&d, "nocontext", counts[ROBUST_COST_NOCONTEXT]);
	robust_dump_int(&d, "projected", counts[ROBUST_COST_PROJECTED]);
	robust_dump_int(&d, "flips", robust_cost_round_flips());
	robust_dump_end(&d);
}

/* ------------------------------------------------------- entry points */

/*
 * robust_search_begin
 *		A join search over initial_rels starts (in the base pass).
 */
RobustSearch *
robust_search_begin(PlannerInfo *root, List *initial_rels)
{
	RobustSearch *rs = palloc0(sizeof(RobustSearch));
	HASHCTL		ctl;
	ListCell   *lc;

	rs->root = root;
	rs->cxt = CurrentMemoryContext;
	rs->initial_rels = initial_rels;
	rs->round = -1;
	rs->select = robust_select;
	root->robust_search = rs;

	/*
	 * A round view projects every base and other-member relation (the cost
	 * functions read the rows of base relations; partitionwise joins use
	 * the partitions) and every join relation built so far (the results of
	 * earlier subproblems of make_rel_from_joinlist, and their partitions).
	 */
	for (int i = 1; i < root->simple_rel_array_size; i++)
	{
		if (root->simple_rel_array[i] != NULL)
			rs->view_rels = lappend(rs->view_rels, root->simple_rel_array[i]);
	}
	foreach(lc, root->join_rel_list)
		rs->view_rels = lappend(rs->view_rels, lfirst(lc));

	snapshot_rinfos(rs);

	ctl.keysize = sizeof(Relids);
	ctl.entrysize = sizeof(EnvEntry);
	ctl.hash = bitmap_hash;
	ctl.match = bitmap_match;
	ctl.hcxt = CurrentMemoryContext;
	rs->envelope = hash_create("RobDP envelope", 256, &ctl,
							   HASH_ELEM | HASH_FUNCTION | HASH_COMPARE |
							   HASH_CONTEXT);

	if (robust_debug_dump)
	{
		foreach(lc, initial_rels)
			robust_dump_pathlist(root, (RelOptInfo *) lfirst(lc));
	}
	return rs;
}

/*
 * robust_search_rounds
 *		Does the search run the envelope pass?  When robust_envelope is on
 *		and the paths are not selected, or are selected by an objective that
 *		compares them to the envelope.  (An error if such an objective is
 *		set with robust_envelope off.)
 */
bool
robust_search_rounds(RobustSearch *rs)
{
	bool		needed;

	if (!rs->select)
		return robust_envelope;
	needed = robust_objective_needs_envelope(main_objective_id) ||
		(retain_path_limit > 0 &&
		 robust_objective_needs_envelope(retain_strategy_id));
	if (needed && !robust_envelope)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the objectives of RobDP's selection need the envelope pass"),
				 errhint("Set robust_envelope on, or choose a main objective among 2-5, 8, 17, 18 and a retain strategy among those or 7, 16.")));
	return needed;
}

/*
 * robust_round_begin
 *		Envelope round s starts, in the round's memory context: project the
 *		level-1 relations onto s and record their C*(U, s).
 */
void
robust_round_begin(RobustSearch *rs, int s)
{
	ListCell   *lc;

	if (rs->cost_save == NULL)
	{
		MemoryContext old = MemoryContextSwitchTo(rs->cxt);

		rs->cost_save = robust_cost_search_save();
		MemoryContextSwitchTo(old);
	}
	rs->round = s;
	reset_rinfos(rs);
	foreach(lc, rs->view_rels)
		view_rel(rs, (RelOptInfo *) lfirst(lc), s);
	robust_cost_round_project(s);
	foreach(lc, rs->initial_rels)
		record(rs, (RelOptInfo *) lfirst(lc), 1, false);
}

/* A relation of the round is complete (set_cheapest done) */
void
robust_round_record(RobustSearch *rs, RelOptInfo *rel)
{
	record(rs, rel, bms_num_members(rel->relids), false);
}

/*
 * robust_round_end
 *		Put the level-1 relations back, and robustcost.c's state.  To be
 *		called before the round's memory is freed.
 */
void
robust_round_end(RobustSearch *rs)
{
	robust_cost_round_project(-1);
	unview(rs);
	robust_cost_search_restore(rs->cost_save);
	rs->round = -1;
}

/*
 * robust_main_begin
 *		The main pass starts: the caches as before the envelope rounds.
 */
void
robust_main_begin(RobustSearch *rs)
{
	ListCell   *lc;

	if (rs->env_order != NIL)
	{
		reset_rinfos(rs);
		if (robust_debug_dump)
			dump_envelope(rs);
	}
	if (!rs->select)
		return;

	/*
	 * Score the level-1 relations' paths, so that their cheapest paths are
	 * the best by the objective, without pruning them (v20 selected join
	 * relations only).  The results of earlier subproblems are scored
	 * already.
	 */
	foreach(lc, rs->initial_rels)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(lc);
		Path	   *first = rel->pathlist != NIL ?
			(Path *) linitial(rel->pathlist) : NULL;

		if (first == NULL || (first->robust != NULL && first->robust->scored))
			continue;
		robust_select_paths(rs->root, rel, true,
							list_length(rel->partial_pathlist), 0,
							envelope_of(rs, rel, true));
		robust_select_paths(rs->root, rel, false,
							list_length(rel->pathlist), 0,
							envelope_of(rs, rel, false));
		set_cheapest(rel);
	}
}

/*
 * With robust_gather_all on: a plain Gather on each partial path candidate
 * of 'partials' (the partial pathlist before the selection) but the first
 * kept one, which generate_useful_gather_paths has gathered already.  They
 * compete with the relation's complete paths in its complete selection.
 */
static void
gather_all_partials(PlannerInfo *root, RelOptInfo *rel, List *partials)
{
	Path	   *first = rel->partial_pathlist != NIL ?
		(Path *) linitial(rel->partial_pathlist) : NULL;
	ListCell   *lc;

	foreach(lc, partials)
	{
		Path	   *subpath = (Path *) lfirst(lc);

		if (subpath == first)
			continue;
		add_path(rel, (Path *) create_gather_path(root, rel, subpath,
												  rel->reltarget, NULL, NULL));
	}
}

/*
 * Select the partial paths of a relation of the main pass, build the Gather
 * paths on those kept (if 'gather'; with robust_gather_all a plain Gather on
 * every partial candidate), select the complete paths, and set its cheapest
 * paths.  At the last level of the search ('top') the main objective keeps
 * robust_final_path_limit paths of each kind (1, as in v20) and the retain
 * strategy none.
 */
static void
select_rel(RobustSearch *rs, RelOptInfo *rel, bool top, bool gather)
{
	int			limit = top ? robust_final_path_limit : add_path_limit;
	int			retain = top ? 0 : retain_path_limit;
	List	   *partials = NIL;

	if (gather && robust_gather_all)
		partials = list_copy(rel->partial_pathlist);
	robust_select_paths(rs->root, rel, true, limit, retain,
						envelope_of(rs, rel, true));
	if (gather)
	{
		generate_useful_gather_paths(rs->root, rel, false);
		gather_all_partials(rs->root, rel, partials);
		list_free(partials);
	}
	robust_select_paths(rs->root, rel, false, limit, retain,
						envelope_of(rs, rel, false));
	set_cheapest(rel);
}

/*
 * robust_search_finish_rel
 *		A join relation of the search is complete (its partitionwise join
 *		paths added): generate its Gather paths, unless 'top' is the relation
 *		of all the query's relations, and set its cheapest paths, as
 *		standard_join_search does; in the main pass of a selecting search,
 *		select its paths too (select_rel).  'top': the last level.
 */
void
robust_search_finish_rel(RobustSearch *rs, RelOptInfo *rel, bool top)
{
	PlannerInfo *root = rs->root;
	bool		gather = !bms_equal(rel->relids, root->all_query_rels);

	if (root->robust_pass == ROBUST_PASS_MAIN && rs->select)
		select_rel(rs, rel, top, gather);
	else
	{
		if (gather)
			generate_useful_gather_paths(root, rel, false);
		set_cheapest(rel);
	}
	if (root->robust_pass == ROBUST_PASS_ENVELOPE)
		robust_round_record(rs, rel);
	else if (robust_debug_dump)
		robust_dump_pathlist(root, rel);
}

/*
 * robust_search_child_rel
 *		generate_partitionwise_join_paths() is done with a child join: set
 *		its cheapest paths; in a selecting search, record its envelope in a
 *		round and select its paths in the main pass (no Gather: the parent's
 *		Append combines the children's partial paths).
 */
void
robust_search_child_rel(RobustSearch *rs, RelOptInfo *child)
{
	PlannerInfo *root = rs->root;

	if (!rs->select)
	{
		set_cheapest(child);
		return;
	}
	if (root->robust_pass == ROBUST_PASS_MAIN)
		select_rel(rs, child, false, false);
	else
		set_cheapest(child);
	if (root->robust_pass == ROBUST_PASS_ENVELOPE)
		record(rs, child, bms_num_members(child->relids), true);
	else if (root->robust_pass == ROBUST_PASS_MAIN && robust_debug_dump)
		robust_dump_pathlist(root, child);
}

/*
 * robust_dump_pathlist
 *		The paths a relation keeps: ids (robustcost.c) of its pathlist and
 *		partial_pathlist, and of its cheapest total path.
 */
void
robust_dump_pathlist(PlannerInfo *root, RelOptInfo *rel)
{
	RobustDump	d;
	StringInfoData buf;

	robust_dump_begin(&d, "pathlist");
	robust_dump_str(&d, "pass", robust_pass_name(root->robust_pass));
	robust_dump_relids(&d, "rels", root, rel->relids);
	initStringInfo(&buf);
	for (int partial = 0; partial < 2; partial++)
	{
		ListCell   *lc;

		resetStringInfo(&buf);
		appendStringInfoChar(&buf, '[');
		foreach(lc, partial ? rel->partial_pathlist : rel->pathlist)
		{
			Path	   *p = (Path *) lfirst(lc);

			if (foreach_current_index(lc) > 0)
				appendStringInfoChar(&buf, ',');
			if (p->robust != NULL)
				appendStringInfo(&buf, "%d", p->robust->id);
			else
				appendStringInfoString(&buf, "null");
		}
		appendStringInfoChar(&buf, ']');
		robust_dump_raw(&d, partial ? "partial" : "paths", buf.data);
	}
	pfree(buf.data);
	if (rel->cheapest_total_path != NULL &&
		rel->cheapest_total_path->robust != NULL)
		robust_dump_int(&d, "cheapest_total", rel->cheapest_total_path->robust->id);
	robust_dump_end(&d);
}

void
robust_search_end(RobustSearch *rs)
{
	rs->root->robust_search = NULL;
	hash_destroy(rs->rinfos);
	hash_destroy(rs->envelope);
	pfree(rs);
}
