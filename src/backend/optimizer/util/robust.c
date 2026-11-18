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
#include "utils/json.h"

/* GUC parameters, see guc_tables.c */
bool		enable_rows_dist = false;
int			error_sample_count = 20;
int			error_sample_seed = 42;
int			error_bin_count = 1;
double		error_sample_kde_bandwidth = 0.0;
char	   *error_profile_path = NULL;
bool		robust_debug_dump = false;

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

static void
dump_number(StringInfo buf, double val)
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
	dump_number(&d->buf, val);
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
		dump_number(&d->buf, v->v[i]);
	}
	appendStringInfoChar(&d->buf, ']');
}

static int
cmp_cstring(const void *a, const void *b)
{
	return strcmp(*(const char *const *) a, *(const char *const *) b);
}

/*
 * robust_dump_relids
 *		A set of relations as the sorted, space-separated list of their
 *		range-table aliases, e.g. "t1 t2".  Outer-join relids (RTE_JOIN
 *		entries) are left out.  NULL relids are written as null.
 */
void
robust_dump_relids(RobustDump *d, const char *key, PlannerInfo *root,
				   Relids relids)
{
	const char **names;
	int			n = 0;
	int			relid = -1;
	StringInfoData s;

	if (relids == NULL)
	{
		robust_dump_str(d, key, NULL);
		return;
	}
	names = palloc(sizeof(char *) * bms_num_members(relids));
	while ((relid = bms_next_member(relids, relid)) >= 0)
	{
		RangeTblEntry *rte;

		if (relid >= root->simple_rel_array_size)
			continue;
		rte = root->simple_rte_array[relid];
		if (rte == NULL || rte->rtekind == RTE_JOIN)
			continue;
		names[n++] = rte->eref->aliasname;
	}
	qsort(names, n, sizeof(char *), cmp_cstring);

	initStringInfo(&s);
	for (int i = 0; i < n; i++)
	{
		if (i > 0)
			appendStringInfoChar(&s, ' ');
		appendStringInfoString(&s, names[i]);
	}
	robust_dump_str(d, key, s.data);
	pfree(s.data);
	pfree(names);
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
