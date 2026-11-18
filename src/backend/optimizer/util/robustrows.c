/*-------------------------------------------------------------------------
 *
 * robustrows.c
 *	  Row counts of relations in the error scenarios of RobDP
 *	  (see robustrows.h for the model).
 *
 * Querylets.  A base relation's restriction clauses form one querylet,
 * named by the relation's alias.  The join clauses between two base
 * relations form one querylet, named "a=b" with the two aliases in sorted
 * order.  Join clauses that reference three or more base relations belong
 * to no querylet.  An inheritance child (a partition) uses the querylets of
 * its topmost parent, since its restrictions are the parent's.
 *
 * Profiles.  The profile of a querylet is "<alias>", else "<std alias>"
 * where the standard alias is the alias cut at its first digit ("mc1" ->
 * "mc"), as in v20; for a join, "a=b" else "std(a)=std(b)", each in sorted
 * order.
 *
 * Instances.  The draws of a querylet are seeded by its instance name: the
 * relation's robust_rel_name() (the alias, made unique within the query
 * level by "#<rti>" when needed), or "ia=ib" for a join.  So the two sides
 * of a self-join, or two references to a table under the same alias, draw
 * independently even when they share a profile.
 *
 * Floating point.  Products and the clamping are written one operation per
 * statement, in the order of the querylet list, so the harness can
 * recompute every row count bit for bit.
 *
 * src/backend/optimizer/util/robustrows.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/robust.h"
#include "optimizer/robustprofile.h"
#include "optimizer/robustrows.h"
#include "utils/json.h"
#include "utils/selfuncs.h"

/* The row-count vector of a relation: rel->robust_rows, or rel->rows */
RobustVec *
robust_rel_rows(const RelOptInfo *rel)
{
	if (rel->robust_rows != NULL)
		return rel->robust_rows;
	return robust_vec_const(rel->rows);
}

/* ---------------------------------------------------------------- names */

/* The alias cut at its first digit; NULL if that leaves nothing */
static char *
std_alias(const char *alias)
{
	size_t		len = strcspn(alias, "0123456789");

	if (len == 0)
		return NULL;
	return pnstrdup(alias, len);
}

/* "a=b" with a <= b */
static char *
pair_name(const char *a, const char *b)
{
	if (strcmp(a, b) <= 0)
		return psprintf("%s=%s", a, b);
	return psprintf("%s=%s", b, a);
}

/* -------------------------------------------------------------- querylets */

/*
 * Draw the errors of a querylet: profile key, else fallback (if different).
 * NULL if neither profile exists.
 */
static RobustQuerylet *
make_querylet(const char *key, const char *fallback, const char *inst,
			  double sel_est)
{
	const RobustProfile *prof = robust_get_profile(key);
	RobustQuerylet *q;
	RobustVec  *x;
	int			bin;

	if (prof == NULL && fallback != NULL && strcmp(fallback, key) != 0)
		prof = robust_get_profile(fallback);
	if (prof == NULL)
		return NULL;
	x = robust_draw_errors(prof, inst, sel_est, &bin);
	if (x == NULL)
		return NULL;

	q = palloc(sizeof(RobustQuerylet));
	q->key = pstrdup(prof->key);
	q->inst = pstrdup(inst);
	q->sel_est = sel_est;
	q->bin = bin;
	q->x = x;
	q->mult = robust_vec_make(x->n);
	for (int s = 0; s < x->n; s++)
	{
		double		e = exp(x->v[s]);
		double		t;

		if (sel_est > 0.0)
		{
			t = sel_est * e;
			CLAMP_PROBABILITY(t);
			q->mult->v[s] = t / sel_est;
		}
		else
			q->mult->v[s] = 1.0;
	}
	return q;
}

/*
 * robust_base_querylet
 *		The querylet of the restriction clauses of base relation rel, whose
 *		selectivity PostgreSQL estimates as sel_est; NULL if it has no
 *		restriction clause or no profile.
 */
RobustQuerylet *
robust_base_querylet(PlannerInfo *root, RelOptInfo *rel, double sel_est)
{
	Index		r;
	const char *alias;
	char	   *inst;
	RobustQuerylet *q;

	if (rel->baserestrictinfo == NIL)
		return NULL;
	r = robust_instance_relid(root, rel->relid);
	alias = root->simple_rte_array[r]->eref->aliasname;
	inst = robust_rel_name(root, r);
	q = make_querylet(alias, std_alias(alias), inst, sel_est);
	pfree(inst);
	return q;
}

/* The join clauses between two relation instances */
typedef struct ClauseGroup
{
	Index		a;				/* instance relids, a < b */
	Index		b;
	Relids		arels;			/* the base relids of a's side */
	Relids		brels;
	List	   *clauses;
} ClauseGroup;

/*
 * robust_join_querylets
 *		The querylets among a list of join clauses (RestrictInfos) that have
 *		a profile, in the order their first clause appears.
 *
 * Each querylet's own estimate is the inner-join selectivity of its
 * clauses, computed the same way wherever the querylet appears, so the
 * same querylet always draws from the same bin.
 */
List *
robust_join_querylets(PlannerInfo *root, List *clauses)
{
	List	   *groups = NIL;
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, clauses)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		Relids		rels = bms_difference(rinfo->clause_relids, root->outer_join_rels);
		int			r1;
		int			r2;
		Index		i1;
		Index		i2;
		ClauseGroup *g = NULL;
		ListCell   *lg;

		if (bms_num_members(rels) != 2)
			continue;
		r1 = bms_next_member(rels, -1);
		r2 = bms_next_member(rels, r1);
		i1 = robust_instance_relid(root, r1);
		i2 = robust_instance_relid(root, r2);
		if (i1 == i2)
			continue;
		if (i1 > i2)
		{
			Index		ti = i1;
			int			tr = r1;

			i1 = i2;
			i2 = ti;
			r1 = r2;
			r2 = tr;
		}
		foreach(lg, groups)
		{
			ClauseGroup *cand = (ClauseGroup *) lfirst(lg);

			if (cand->a == i1 && cand->b == i2)
			{
				g = cand;
				break;
			}
		}
		if (g == NULL)
		{
			g = palloc0(sizeof(ClauseGroup));
			g->a = i1;
			g->b = i2;
			groups = lappend(groups, g);
		}
		g->arels = bms_add_member(g->arels, r1);
		g->brels = bms_add_member(g->brels, r2);
		g->clauses = lappend(g->clauses, rinfo);
	}

	foreach(lc, groups)
	{
		ClauseGroup *g = (ClauseGroup *) lfirst(lc);
		const char *a = root->simple_rte_array[g->a]->eref->aliasname;
		const char *b = root->simple_rte_array[g->b]->eref->aliasname;
		char	   *sa = std_alias(a);
		char	   *sb = std_alias(b);
		char	   *ia = robust_rel_name(root, g->a);
		char	   *ib = robust_rel_name(root, g->b);
		SpecialJoinInfo sjinfo;
		Selectivity sel;
		RobustQuerylet *q;

		/* A dummy inner join of the two sides, as approx_tuple_count() makes */
		sjinfo.type = T_SpecialJoinInfo;
		sjinfo.min_lefthand = g->arels;
		sjinfo.min_righthand = g->brels;
		sjinfo.syn_lefthand = g->arels;
		sjinfo.syn_righthand = g->brels;
		sjinfo.jointype = JOIN_INNER;
		sjinfo.ojrelid = 0;
		sjinfo.commute_above_l = NULL;
		sjinfo.commute_above_r = NULL;
		sjinfo.commute_below_l = NULL;
		sjinfo.commute_below_r = NULL;
		sjinfo.lhs_strict = false;
		sjinfo.semi_can_btree = false;
		sjinfo.semi_can_hash = false;
		sjinfo.semi_operators = NIL;
		sjinfo.semi_rhs_exprs = NIL;
		sel = clauselist_selectivity(root, g->clauses, 0, JOIN_INNER, &sjinfo);

		q = make_querylet(pair_name(a, b),
						  (sa && sb) ? pair_name(sa, sb) : NULL,
						  pair_name(ia, ib), sel);
		if (q != NULL)
			result = lappend(result, q);
	}
	return result;
}

/* -------------------------------------------------------------- dumping */

static void
dump_querylets(RobustDump *d, List *querylets)
{
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	foreach(lc, querylets)
	{
		RobustQuerylet *q = (RobustQuerylet *) lfirst(lc);

		if (foreach_current_index(lc) > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, "{\"key\":");
		escape_json(&buf, q->key);
		appendStringInfoString(&buf, ",\"inst\":");
		escape_json(&buf, q->inst);
		appendStringInfoString(&buf, ",\"sel_est\":");
		robust_json_number(&buf, q->sel_est);
		appendStringInfo(&buf, ",\"bin\":%d}", q->bin);
	}
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(d, "querylets", buf.data);
	pfree(buf.data);
}

static void
dump_rel(PlannerInfo *root, RelOptInfo *rel, const char *kind, double sel_est,
		 List *children)
{
	RobustDump	d;

	robust_dump_begin(&d, "rel");
	robust_dump_str(&d, "pass", robust_pass_name(root->robust_pass));
	robust_dump_relids(&d, "rels", root, rel->relids);
	robust_dump_str(&d, "kind", kind);
	robust_dump_double(&d, "tuples", rel->tuples);
	if (!isnan(sel_est))
		robust_dump_double(&d, "sel_est", sel_est);
	dump_querylets(&d, rel->robust_querylets);
	if (children != NIL)
	{
		StringInfoData buf;
		ListCell   *lc;

		initStringInfo(&buf);
		appendStringInfoChar(&buf, '[');
		foreach(lc, children)
		{
			RelOptInfo *child = (RelOptInfo *) lfirst(lc);

			if (foreach_current_index(lc) > 0)
				appendStringInfoChar(&buf, ',');
			escape_json(&buf, robust_relids_name(root, child->relids));
		}
		appendStringInfoChar(&buf, ']');
		robust_dump_raw(&d, "children", buf.data);
		pfree(buf.data);
	}
	robust_dump_double(&d, "rows_point", rel->rows);
	robust_dump_vec(&d, "rows", robust_rel_rows(rel));
	robust_dump_end(&d);
}

/* ----------------------------------------------------------- row counts */

/*
 * robust_set_baserel_rows
 *		The row vector of a base relation (or inheritance child, or other
 *		relation sized by set_baserel_size_estimates), whose restriction
 *		selectivity PostgreSQL estimates as sel:
 *
 *		rows[s] = clamp_row_est(tuples * clamp01(sel * m[s]))
 */
void
robust_set_baserel_rows(PlannerInfo *root, RelOptInfo *rel, Selectivity sel)
{
	RobustQuerylet *q = robust_base_querylet(root, rel, sel);

	rel->robust_rows = NULL;
	rel->robust_querylets = NIL;
	if (q != NULL)
	{
		RobustVec  *rows = robust_vec_make(q->mult->n);

		for (int s = 0; s < rows->n; s++)
		{
			double		p = sel * q->mult->v[s];
			double		nrows;

			CLAMP_PROBABILITY(p);
			nrows = rel->tuples * p;
			rows->v[s] = clamp_row_est(nrows);
		}
		rel->robust_rows = rows;
		rel->robust_querylets = list_make1(q);
	}

	if (robust_debug_dump)
		dump_rel(root, rel,
				 rel->reloptkind == RELOPT_BASEREL ? "base" : "other",
				 sel, NIL);
}

/*
 * robust_set_appendrel_rows
 *		The row vector of an append relation: the sum of its live children's,
 *		in the order set_append_rel_size() sums their rows.
 */
void
robust_set_appendrel_rows(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *children = NIL;
	bool		varies = false;
	ListCell   *lc;

	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);
		RelOptInfo *child;

		if (appinfo->parent_relid != rel->relid)
			continue;
		child = find_base_rel(root, appinfo->child_relid);
		if (IS_DUMMY_REL(child))
			continue;
		children = lappend(children, child);
		varies |= child->robust_rows != NULL;
	}

	rel->robust_rows = NULL;
	rel->robust_querylets = NIL;
	if (varies)
	{
		RobustVec  *rows = robust_vec_make(error_sample_count);

		for (int s = 0; s < rows->n; s++)
			rows->v[s] = 0;
		foreach(lc, children)
		{
			RobustVec  *c = robust_rel_rows((RelOptInfo *) lfirst(lc));

			for (int s = 0; s < rows->n; s++)
				rows->v[s] += robust_vec_at(c, s);
		}
		rel->robust_rows = rows;
	}

	if (robust_debug_dump)
		dump_rel(root, rel, "append", NAN, children);
	list_free(children);
}

/*
 * robust_set_baserel_ppi_rows
 *		The row vector of a parameterized scan of base relation rel, whose
 *		selectivity (of its restrictions and the join clauses ppi enforces)
 *		PostgreSQL estimates as sel:
 *
 *		rows[s] = min(clamp_row_est(tuples * clamp01(sel * m_1[s] * ...)),
 *					  rel rows[s])
 *
 * over the relation's restriction querylet and the join querylets among
 * ppi's clauses, mirroring get_parameterized_baserel_size().
 */
void
robust_set_baserel_ppi_rows(PlannerInfo *root, RelOptInfo *rel,
							ParamPathInfo *ppi, Selectivity sel)
{
	List	   *querylets = list_concat_copy(rel->robust_querylets,
											 robust_join_querylets(root, ppi->ppi_clauses));

	ppi->ppi_robust_rows = NULL;
	ppi->ppi_robust_querylets = querylets;
	if (querylets != NIL)
	{
		RobustVec  *relrows = robust_rel_rows(rel);
		RobustVec  *rows = robust_vec_make(error_sample_count);

		for (int s = 0; s < rows->n; s++)
		{
			double		p = sel;
			double		nrows;
			ListCell   *lc;

			foreach(lc, querylets)
			{
				RobustQuerylet *q = (RobustQuerylet *) lfirst(lc);

				p = p * q->mult->v[s];
			}
			CLAMP_PROBABILITY(p);
			nrows = rel->tuples * p;
			nrows = clamp_row_est(nrows);
			if (nrows > robust_vec_at(relrows, s))
				nrows = robust_vec_at(relrows, s);
			rows->v[s] = nrows;
		}
		ppi->ppi_robust_rows = rows;
	}

	if (robust_debug_dump)
	{
		RobustDump	d;

		robust_dump_begin(&d, "ppi");
		robust_dump_relids(&d, "rels", root, rel->relids);
		robust_dump_relids(&d, "outer", root, ppi->ppi_req_outer);
		robust_dump_double(&d, "tuples", rel->tuples);
		robust_dump_double(&d, "sel_est", sel);
		dump_querylets(&d, querylets);
		robust_dump_double(&d, "rows_point", ppi->ppi_rows);
		robust_dump_vec(&d, "rows",
						ppi->ppi_robust_rows ? ppi->ppi_robust_rows :
						robust_vec_const(ppi->ppi_rows));
		robust_dump_end(&d);
	}
}
