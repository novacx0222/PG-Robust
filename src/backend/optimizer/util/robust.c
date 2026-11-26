/*-------------------------------------------------------------------------
 *
 * robust.c
 *	  Settings and debug output of RobDP robust planning (see robust.h).
 *
 * src/backend/optimizer/util/robust.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "nodes/bitmapset.h"
#include "optimizer/robust.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/json.h"

/* GUC parameters, see guc_tables.c */
bool		enable_rows_dist = false;
int			error_sample_count = 20;
int			error_sample_seed = 42;
int			error_bin_count = 1;
double		error_sample_kde_bandwidth = 0.0;
char	   *error_profile_path = NULL;
bool		robust_debug_dump = false;
bool		robust_search = true;
bool		robust_envelope = true;
bool		robust_select_dedup = true;
bool		robust_select_precheck = true;
bool		robust_vector_cost = true;
bool		robust_debug_check_costs = DEFAULT_ROBUST_CHECK_COSTS;
bool		robust_gather_all = false;
bool		robust_select = true;
bool		robust_dominance = false;
int			robust_join_pairing = ROBUST_PAIRING_RETAINED;
int			add_path_limit = 1;
int			retain_path_limit = 1;
int			robust_final_path_limit = 1;
int			main_objective_id = 0;
int			retain_strategy_id = 0;
double		robust_penalty_tau = 0.2;
double		robust_penalty_lambda = 1.0;
double		robust_quantile_alpha = 0.85;

const char *
robust_pass_name(RobustPass pass)
{
	switch (pass)
	{
		case ROBUST_PASS_NONE:
			return "none";
		case ROBUST_PASS_BASE:
			return "base";
		case ROBUST_PASS_ENVELOPE:
			return "envelope";
		case ROBUST_PASS_MAIN:
			return "main";
		case ROBUST_PASS_UPPER:
			return "upper";
	}
	return "?";
}

/* ------------------------------------------------------------------------
 * Debug output
 *
 * Numbers are printed with %.17g so that the harness reads back the exact
 * double.  JSON has no NaN or infinity; those are written as the strings
 * "nan", "inf" and "-inf".
 * ------------------------------------------------------------------------
 */

static void
dump_key(RobustDump *d, const char *key)
{
	if (d->buf.data[d->buf.len - 1] != '{')
		appendStringInfoChar(&d->buf, ',');
	escape_json(&d->buf, key);
	appendStringInfoChar(&d->buf, ':');
}

void
robust_json_number(StringInfo buf, double val)
{
	if (isnan(val))
		appendStringInfoString(buf, "\"nan\"");
	else if (isinf(val))
		appendStringInfoString(buf, val > 0 ? "\"inf\"" : "\"-inf\"");
	else
		appendStringInfo(buf, "%.17g", val);
}

void
robust_dump_begin(RobustDump *d, const char *ev)
{
	initStringInfo(&d->buf);
	appendStringInfoChar(&d->buf, '{');
	robust_dump_str(d, "ev", ev);
}

void
robust_dump_str(RobustDump *d, const char *key, const char *val)
{
	dump_key(d, key);
	if (val == NULL)
		appendStringInfoString(&d->buf, "null");
	else
		escape_json(&d->buf, val);
}

void
robust_dump_int(RobustDump *d, const char *key, int64 val)
{
	dump_key(d, key);
	appendStringInfo(&d->buf, INT64_FORMAT, val);
}

void
robust_dump_double(RobustDump *d, const char *key, double val)
{
	dump_key(d, key);
	robust_json_number(&d->buf, val);
}

/* A value that is already JSON text (an array or object) */
void
robust_dump_raw(RobustDump *d, const char *key, const char *json)
{
	dump_key(d, key);
	appendStringInfoString(&d->buf, json);
}

void
robust_dump_bool(RobustDump *d, const char *key, bool val)
{
	dump_key(d, key);
	appendStringInfoString(&d->buf, val ? "true" : "false");
}

void
robust_dump_vec(RobustDump *d, const char *key, const RobustVec *v)
{
	dump_key(d, key);
	if (v == NULL)
	{
		appendStringInfoString(&d->buf, "null");
		return;
	}
	appendStringInfoChar(&d->buf, '[');
	for (int i = 0; i < v->n; i++)
	{
		if (i > 0)
			appendStringInfoChar(&d->buf, ',');
		robust_json_number(&d->buf, v->v[i]);
	}
	appendStringInfoChar(&d->buf, ']');
}

static int
cmp_cstring(const void *a, const void *b)
{
	return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/*
 * Is range-table entry rti a child of an inheritance (or partitioned) parent?
 * Children of UNION ALL subqueries are not: they are relations of their own.
 */
static bool
is_inheritance_child(PlannerInfo *root, Index rti)
{
	AppendRelInfo *appinfo;

	if (root->append_rel_array == NULL)
		return false;
	appinfo = root->append_rel_array[rti];
	return appinfo != NULL &&
		root->simple_rte_array[appinfo->parent_relid]->rtekind == RTE_RELATION;
}

/*
 * robust_instance_relid
 *		The relation whose querylets relid's querylets are: relid itself, or
 *		for an inheritance child its topmost inheritance parent, since a
 *		partition applies its parent's restrictions.
 */
Index
robust_instance_relid(PlannerInfo *root, Index relid)
{
	while (is_inheritance_child(root, relid))
		relid = root->append_rel_array[relid]->parent_relid;
	return relid;
}

/*
 * robust_rel_name
 *		A name for base relation relid that is unique within its query level.
 *
 * It is the range-table alias, followed by "#<rti>" if another entry of the
 * range table has the same alias (a table referenced twice without
 * aliases, or a pulled-up subquery reusing an alias).  Inheritance children
 * carry their parent's alias, so they are named "<parent>:<relation>",
 * e.g. "p:p_1", and do not count as duplicates of their parent.  RTE_JOIN
 * entries are ignored.
 */
char *
robust_rel_name(PlannerInfo *root, Index relid)
{
	RangeTblEntry *rte = root->simple_rte_array[relid];
	const char *alias = rte->eref->aliasname;
	bool		dup = false;

	if (is_inheritance_child(root, relid))
	{
		char	   *relname = rte->relid != InvalidOid ? get_rel_name(rte->relid) : NULL;

		return psprintf("%s:%s",
						robust_rel_name(root, root->append_rel_array[relid]->parent_relid),
						relname ? relname : "?");
	}
	for (int i = 1; i < root->simple_rel_array_size && !dup; i++)
	{
		RangeTblEntry *other = root->simple_rte_array[i];

		if (i == relid || other == NULL || other->rtekind == RTE_JOIN ||
			is_inheritance_child(root, i))
			continue;
		dup = strcmp(other->eref->aliasname, alias) == 0;
	}
	return dup ? psprintf("%s#%u", alias, relid) : pstrdup(alias);
}

/*
 * robust_relids_name
 *		A set of relations as the sorted, space-separated list of their
 *		robust_rel_name()s, e.g. "t1 t2".  Outer-join relids (RTE_JOIN
 *		entries) are left out.  NULL for NULL relids.
 */
char *
robust_relids_name(PlannerInfo *root, Relids relids)
{
	char	  **names;
	int			n = 0;
	int			relid = -1;
	StringInfoData s;

	if (relids == NULL)
		return NULL;
	names = palloc(sizeof(char *) * bms_num_members(relids));
	while ((relid = bms_next_member(relids, relid)) >= 0)
	{
		RangeTblEntry *rte;

		if (relid >= root->simple_rel_array_size)
			continue;
		rte = root->simple_rte_array[relid];
		if (rte == NULL || rte->rtekind == RTE_JOIN)
			continue;
		names[n++] = robust_rel_name(root, relid);
	}
	qsort(names, n, sizeof(char *), cmp_cstring);

	initStringInfo(&s);
	for (int i = 0; i < n; i++)
	{
		if (i > 0)
			appendStringInfoChar(&s, ' ');
		appendStringInfoString(&s, names[i]);
		pfree(names[i]);
	}
	pfree(names);
	return s.data;
}

/* A set of relations, named by robust_relids_name(); null for NULL relids */
void
robust_dump_relids(RobustDump *d, const char *key, PlannerInfo *root,
				   Relids relids)
{
	char	   *name = robust_relids_name(root, relids);

	robust_dump_str(d, key, name);
	if (name)
		pfree(name);
}

void
robust_dump_end(RobustDump *d)
{
	appendStringInfoChar(&d->buf, '}');
	elog(LOG, "ROBDP %s", d->buf.data);
	pfree(d->buf.data);
}

/*
 * robust_dump_query
 *		Emit the "query" event: the robust planning settings in effect when
 *		a query is planned.  Called from standard_planner().
 */
void
robust_dump_query(void)
{
	RobustDump	d;

	robust_dump_begin(&d, "query");
	robust_dump_bool(&d, "enabled", enable_rows_dist);
	robust_dump_int(&d, "N", error_sample_count);
	robust_dump_int(&d, "seed", error_sample_seed);
	robust_dump_int(&d, "bins", error_bin_count);
	robust_dump_double(&d, "bandwidth", error_sample_kde_bandwidth);
	robust_dump_str(&d, "profile_path", error_profile_path);
	robust_dump_end(&d);
}
