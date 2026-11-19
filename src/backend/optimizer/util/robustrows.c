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
 * its topmost parent, since its restrictions are the parent's.  The clauses
 * PostgreSQL generates from an equivalence class with 3 or more member
 * relations belong to no clause querylet either: the class enters through
 * the spanning forests of robust_ec_factor() (see robustrows.h), whose pair
 * querylets are named like join querylets.
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

#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/robust.h"
#include "optimizer/robustprofile.h"
#include "optimizer/robustrows.h"
#include "utils/json.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/selfuncs.h"

/* The row-count vector of a relation: rel->robust_rows, or rel->rows */
RobustVec *
robust_rel_rows(const RelOptInfo *rel)
{
	if (rel->robust_rows != NULL)
		return rel->robust_rows;
	return robust_vec_const(rel->rows);
}

/*
 * robust_path_rows
 *		The row-count vector of a path's output, and in *source where it
 *		comes from: "ppi" (a parameterized path whose rows are its
 *		ParamPathInfo's), "rel" (an unparameterized path whose rows are its
 *		relation's), or "const" for any other path (a unique-ified path, for
 *		instance), whose rows are not modeled per scenario yet.  *varies is
 *		false when the vector is the point estimate in every scenario (the
 *		vector's length cannot tell: with N = 1 every vector has length 1).
 */
RobustVec *
robust_path_rows(const Path *path, const char **source, bool *varies)
{
	const ParamPathInfo *ppi = path->param_info;

	if (ppi != NULL)
	{
		if (path->rows == ppi->ppi_rows)
		{
			*source = "ppi";
			*varies = ppi->ppi_robust_rows != NULL;
			return ppi->ppi_robust_rows ? ppi->ppi_robust_rows :
				robust_vec_const(ppi->ppi_rows);
		}
	}
	else if (path->rows == path->parent->rows)
	{
		*source = "rel";
		*varies = path->parent->robust_rows != NULL;
		return robust_rel_rows(path->parent);
	}
	*source = "const";
	*varies = false;
	return robust_vec_const(path->rows);
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
 * The querylets drawn so far at a query level (PlannerInfo.robust_qcache),
 * keyed by the profile keys tried, the instance and PostgreSQL's estimate:
 * a querylet is drawn once, however often it is needed (the dump shows one
 * "draw" event per querylet), and a missing profile is looked up once.
 */
typedef struct QueryletCacheEntry
{
	char	   *key;
	char	   *fallback;		/* NULL: none */
	char	   *inst;
	double		sel_est;
	RobustQuerylet *q;			/* NULL: no profile */
} QueryletCacheEntry;

static bool
same_str(const char *a, const char *b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcmp(a, b) == 0;
}

/*
 * Draw the errors of a querylet: profile key, else fallback (if different).
 * NULL if neither profile exists.  The result is a copy the caller may
 * change (pushed, ia, ib); the draws and multipliers are shared.
 */
static RobustQuerylet *
make_querylet(PlannerInfo *root, const char *key, const char *fallback,
			  const char *inst, double sel_est)
{
	const RobustProfile *prof;
	RobustQuerylet *q;
	RobustQuerylet *copy;
	RobustVec  *x;
	int			bin;
	ListCell   *lc;
	QueryletCacheEntry *ent;
	MemoryContext oldcxt;

	foreach(lc, root->robust_qcache)
	{
		ent = (QueryletCacheEntry *) lfirst(lc);
		if (ent->sel_est == sel_est && strcmp(ent->inst, inst) == 0 &&
			strcmp(ent->key, key) == 0 && same_str(ent->fallback, fallback))
		{
			if (ent->q == NULL)
				return NULL;
			copy = palloc(sizeof(RobustQuerylet));
			memcpy(copy, ent->q, sizeof(RobustQuerylet));
			return copy;
		}
	}

	oldcxt = MemoryContextSwitchTo(GetMemoryChunkContext(root));
	ent = palloc(sizeof(QueryletCacheEntry));
	ent->key = pstrdup(key);
	ent->fallback = fallback ? pstrdup(fallback) : NULL;
	ent->inst = pstrdup(inst);
	ent->sel_est = sel_est;
	ent->q = NULL;
	root->robust_qcache = lappend(root->robust_qcache, ent);

	prof = robust_get_profile(key);
	if (prof == NULL && fallback != NULL && strcmp(fallback, key) != 0)
		prof = robust_get_profile(fallback);
	x = prof ? robust_draw_errors(prof, inst, sel_est, &bin) : NULL;
	if (x != NULL)
	{
		q = palloc0(sizeof(RobustQuerylet));
		q->key = pstrdup(prof->key);
		q->inst = pstrdup(inst);
		q->sel_est = sel_est;
		q->bin = bin;
		q->pushed = false;
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
		ent->q = q;
	}
	MemoryContextSwitchTo(oldcxt);

	if (ent->q == NULL)
		return NULL;
	copy = palloc(sizeof(RobustQuerylet));
	memcpy(copy, ent->q, sizeof(RobustQuerylet));
	return copy;
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
	q = make_querylet(root, alias, std_alias(alias), inst, sel_est);
	pfree(inst);
	if (q != NULL)
		q->ia = r;
	return q;
}

/* A dummy inner join of two sides, as approx_tuple_count() makes */
static void
dummy_inner_sjinfo(SpecialJoinInfo *sjinfo, Relids left, Relids right)
{
	sjinfo->type = T_SpecialJoinInfo;
	sjinfo->min_lefthand = left;
	sjinfo->min_righthand = right;
	sjinfo->syn_lefthand = left;
	sjinfo->syn_righthand = right;
	sjinfo->jointype = JOIN_INNER;
	sjinfo->ojrelid = 0;
	sjinfo->commute_above_l = NULL;
	sjinfo->commute_above_r = NULL;
	sjinfo->commute_below_l = NULL;
	sjinfo->commute_below_r = NULL;
	sjinfo->lhs_strict = false;
	sjinfo->semi_can_btree = false;
	sjinfo->semi_can_hash = false;
	sjinfo->semi_operators = NIL;
	sjinfo->semi_rhs_exprs = NIL;
}

/* ------------------------------------------------- equivalence classes */

/*
 * A pair of member relations of one or more multi-member equivalence
 * classes (see robustrows.h).  Its querylet is the equalities between the
 * two relations, one per class containing both, as if PostgreSQL joined
 * the two directly.
 */
typedef struct ECPair
{
	Index		a;				/* instance relids, a < b */
	Index		b;
	Relids		arels;			/* the relations of a's members */
	Relids		brels;
	List	   *clauses;		/* bare equality OpExprs */
	RobustQuerylet *q;			/* NULL: no profile */
} ECPair;

/* A multi-member equivalence class */
typedef struct ECStar
{
	EquivalenceClass *ec;
	int			k;				/* member relations, best-ranked first: */
	Index	   *rel;			/* instance relid */
	char	  **name;			/* instance name */
	int		   *cover;			/* number of profiled pairs it is in */
	int			norder;			/* the profiled pairs in Kruskal order: */
	int		   *order_pair;		/* index in RobustECGraph.pairs */
	int		   *order_u;		/* endpoints, as member indexes */
	int		   *order_v;
} ECStar;

struct RobustECGraph
{
	List	   *stars;			/* ECStar, in root->eq_classes order */
	int			npairs;
	ECPair	  **pairs;
};

typedef struct ECMember
{
	Index		inst;
	Index		relid;
	EquivalenceMember *em;
} ECMember;

/* select_equality_operator() of equivclass.c */
static Oid
ec_equality_operator(EquivalenceClass *ec, Oid lefttype, Oid righttype)
{
	ListCell   *lc;

	foreach(lc, ec->ec_opfamilies)
	{
		Oid			opno = get_opfamily_member(lfirst_oid(lc), lefttype, righttype,
											   BTEqualStrategyNumber);

		if (!OidIsValid(opno))
			continue;
		if (ec->ec_max_security == 0 || get_func_leakproof(get_opcode(opno)))
			return opno;
	}
	return InvalidOid;
}

static int
ec_find_pair(RobustECGraph *g, Index a, Index b)
{
	for (int p = 0; p < g->npairs; p++)
		if (g->pairs[p]->a == a && g->pairs[p]->b == b)
			return p;
	return -1;
}

/* Kruskal order: better-ranked endpoint, then the other endpoint */
typedef struct ECOrder
{
	int			pair;
	int			u;
	int			v;
} ECOrder;

static int
ec_order_cmp(const void *x, const void *y)
{
	const ECOrder *p = (const ECOrder *) x;
	const ECOrder *q = (const ECOrder *) y;

	if (p->u != q->u)
		return p->u < q->u ? -1 : 1;
	if (p->v != q->v)
		return p->v < q->v ? -1 : 1;
	return 0;
}

static void dump_ec_graph(PlannerInfo *root, RobustECGraph *g);

/*
 * The members of an equivalence class usable for the spanning trees: one
 * per relation (its first single-relation member).  Returns false if the
 * class is not a multi-member join class: it has a constant, a volatile
 * member, is broken (PostgreSQL then applies its original clauses), or a
 * member is nulled by an outer join (PostgreSQL would apply its clauses
 * above that join, not where the member's relation joins).
 */
static bool
ec_members(PlannerInfo *root, EquivalenceClass *ec, ECMember **out, int *k)
{
	ECMember   *m;
	int			n = 0;
	ListCell   *lc;

	if (ec->ec_merged != NULL || ec->ec_has_const || ec->ec_has_volatile ||
		ec->ec_broken)
		return false;
	m = palloc(sizeof(ECMember) * list_length(ec->ec_members));
	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		int			relid;
		bool		dup = false;

		if (em->em_is_child || em->em_is_const)
			continue;
		if (bms_overlap(em->em_relids, root->outer_join_rels))
			return false;
		if (!bms_get_singleton_member(em->em_relids, &relid))
			continue;
		for (int i = 0; i < n; i++)
			dup |= m[i].relid == relid;
		if (dup)
			continue;
		m[n].relid = relid;
		m[n].inst = robust_instance_relid(root, relid);
		m[n].em = em;
		n++;
	}
	*out = m;
	*k = n;
	return n >= 3;
}

/*
 * The spanning-tree data of the query level's multi-member equivalence
 * classes, built on first use after the classes are final.  NULL before
 * that (no class is treated as multi-member then).
 */
static RobustECGraph *
ec_graph(PlannerInfo *root)
{
	RobustECGraph *g;
	MemoryContext old;
	List	   *members = NIL;	/* per star: ECMember array */
	List	   *counts = NIL;
	ListCell   *lc;
	int			allocated = 8;

	if (root->robust_ec_graph != NULL)
		return root->robust_ec_graph;
	if (!root->ec_merging_done)
		return NULL;

	/* GEQO plans in a short-lived context; the graph lives with root */
	old = MemoryContextSwitchTo(GetMemoryChunkContext(root));
	g = palloc0(sizeof(RobustECGraph));
	g->pairs = palloc(sizeof(ECPair *) * allocated);

	foreach(lc, root->eq_classes)
	{
		EquivalenceClass *ec = (EquivalenceClass *) lfirst(lc);
		ECMember   *m;
		int			k;
		ECStar	   *st;

		if (!ec_members(root, ec, &m, &k))
			continue;
		st = palloc0(sizeof(ECStar));
		st->ec = ec;
		st->k = k;
		g->stars = lappend(g->stars, st);
		members = lappend(members, m);
		counts = lappend_int(counts, k);

		/* The equality of every member pair, collected per relation pair */
		for (int i = 0; i < k; i++)
		{
			for (int j = i + 1; j < k; j++)
			{
				ECMember   *x = &m[i];
				ECMember   *y = &m[j];
				Oid			opno;
				ECPair	   *pr;
				int			p;

				if (x->inst == y->inst)
					continue;
				if (x->inst > y->inst)
				{
					ECMember   *t = x;

					x = y;
					y = t;
				}
				opno = ec_equality_operator(ec, x->em->em_datatype,
											y->em->em_datatype);
				if (!OidIsValid(opno))
					continue;
				p = ec_find_pair(g, x->inst, y->inst);
				if (p < 0)
				{
					if (g->npairs == allocated)
					{
						allocated *= 2;
						g->pairs = repalloc(g->pairs, sizeof(ECPair *) * allocated);
					}
					pr = palloc0(sizeof(ECPair));
					pr->a = x->inst;
					pr->b = y->inst;
					g->pairs[g->npairs++] = pr;
				}
				else
					pr = g->pairs[p];
				pr->arels = bms_add_member(pr->arels, x->relid);
				pr->brels = bms_add_member(pr->brels, y->relid);
				pr->clauses = lappend(pr->clauses,
									  make_opclause(opno, BOOLOID, false,
													x->em->em_expr,
													y->em->em_expr,
													InvalidOid,
													ec->ec_collation));
			}
		}
	}

	/* Each pair's querylet, estimated as an inner join of the two */
	for (int p = 0; p < g->npairs; p++)
	{
		ECPair	   *pr = g->pairs[p];
		const char *a = root->simple_rte_array[pr->a]->eref->aliasname;
		const char *b = root->simple_rte_array[pr->b]->eref->aliasname;
		char	   *sa = std_alias(a);
		char	   *sb = std_alias(b);
		SpecialJoinInfo sjinfo;
		Selectivity sel;

		dummy_inner_sjinfo(&sjinfo, pr->arels, pr->brels);
		sel = clauselist_selectivity(root, pr->clauses, 0, JOIN_INNER, &sjinfo);
		pr->q = make_querylet(root, pair_name(a, b),
							  (sa && sb) ? pair_name(sa, sb) : NULL,
							  pair_name(robust_rel_name(root, pr->a),
										robust_rel_name(root, pr->b)),
							  sel);
		if (pr->q != NULL)
		{
			pr->q->ia = pr->a;
			pr->q->ib = pr->b;
		}
	}

	/* Rank each class's members and order its profiled pairs */
	foreach(lc, g->stars)
	{
		ECStar	   *st = (ECStar *) lfirst(lc);
		ECMember   *m = (ECMember *) list_nth(members, foreach_current_index(lc));
		int			k = st->k;
		int		   *cover = palloc0(sizeof(int) * k);
		char	  **name = palloc(sizeof(char *) * k);
		int		   *rank_of = palloc(sizeof(int) * k);
		int		   *by_rank = palloc(sizeof(int) * k);
		ECOrder    *ord = palloc(sizeof(ECOrder) * k * k);
		int			n = 0;

		for (int i = 0; i < k; i++)
			name[i] = robust_rel_name(root, m[i].inst);
		for (int i = 0; i < k; i++)
			for (int j = i + 1; j < k; j++)
			{
				int			p;

				if (m[i].inst == m[j].inst)
					continue;
				p = ec_find_pair(g, Min(m[i].inst, m[j].inst),
								 Max(m[i].inst, m[j].inst));
				if (p >= 0 && g->pairs[p]->q != NULL)
				{
					cover[i]++;
					cover[j]++;
				}
			}

		/* Rank: more profiled pairs first, then by name (insertion sort) */
		for (int i = 0; i < k; i++)
		{
			int			pos = i;

			while (pos > 0)
			{
				int			o = by_rank[pos - 1];

				if (cover[o] > cover[i] ||
					(cover[o] == cover[i] && strcmp(name[o], name[i]) <= 0))
					break;
				by_rank[pos] = o;
				pos--;
			}
			by_rank[pos] = i;
		}
		for (int r = 0; r < k; r++)
			rank_of[by_rank[r]] = r;

		st->rel = palloc(sizeof(Index) * k);
		st->name = palloc(sizeof(char *) * k);
		st->cover = palloc(sizeof(int) * k);
		for (int r = 0; r < k; r++)
		{
			st->rel[r] = m[by_rank[r]].inst;
			st->name[r] = name[by_rank[r]];
			st->cover[r] = cover[by_rank[r]];
		}

		for (int u = 0; u < k; u++)
			for (int v = u + 1; v < k; v++)
			{
				int			p;

				if (st->rel[u] == st->rel[v])
					continue;
				p = ec_find_pair(g, Min(st->rel[u], st->rel[v]),
								 Max(st->rel[u], st->rel[v]));
				if (p < 0 || g->pairs[p]->q == NULL)
					continue;
				ord[n].pair = p;
				ord[n].u = u;
				ord[n].v = v;
				n++;
			}
		qsort(ord, n, sizeof(ECOrder), ec_order_cmp);
		st->norder = n;
		st->order_pair = palloc(sizeof(int) * Max(n, 1));
		st->order_u = palloc(sizeof(int) * Max(n, 1));
		st->order_v = palloc(sizeof(int) * Max(n, 1));
		for (int i = 0; i < n; i++)
		{
			st->order_pair[i] = ord[i].pair;
			st->order_u[i] = ord[i].u;
			st->order_v[i] = ord[i].v;
		}
	}

	root->robust_ec_graph = g;
	MemoryContextSwitchTo(old);

	if (robust_debug_dump && g->stars != NIL)
		dump_ec_graph(root, g);
	return g;
}

/* Is ec a multi-member equivalence class handled by spanning trees? */
bool
robust_ec_is_star(PlannerInfo *root, EquivalenceClass *ec)
{
	RobustECGraph *g = ec_graph(root);
	ListCell   *lc;

	if (g == NULL)
		return false;
	while (ec->ec_merged != NULL)
		ec = ec->ec_merged;
	foreach(lc, g->stars)
		if (((ECStar *) lfirst(lc))->ec == ec)
			return true;
	return false;
}

/* The instance relids of the base relations among relids */
static Bitmapset *
ec_instances(PlannerInfo *root, Relids relids)
{
	Bitmapset  *result = NULL;
	int			r = -1;

	while ((r = bms_next_member(relids, r)) >= 0)
	{
		if (r < root->simple_rel_array_size && root->simple_rel_array[r] != NULL)
			result = bms_add_member(result, robust_instance_relid(root, r));
	}
	return result;
}

static int
ec_root(int *parent, int i)
{
	while (parent[i] != i)
		i = parent[i];
	return i;
}

/* Mark in active[] the edges of st's spanning forest on the instances x */
static void
ec_forest(ECStar *st, Bitmapset *x, bool *active)
{
	int		   *parent = palloc(sizeof(int) * st->k);
	bool	   *in = palloc(sizeof(bool) * st->k);

	for (int i = 0; i < st->k; i++)
	{
		parent[i] = i;
		in[i] = bms_is_member(st->rel[i], x);
	}
	for (int o = 0; o < st->norder; o++)
	{
		int			u = st->order_u[o];
		int			v = st->order_v[o];
		int			ru;
		int			rv;

		if (!in[u] || !in[v])
			continue;
		ru = ec_root(parent, u);
		rv = ec_root(parent, v);
		if (ru == rv)
			continue;
		parent[ru] = rv;
		active[st->order_pair[o]] = true;
	}
	pfree(parent);
	pfree(in);
}

/* Add sign to cnt[] for every edge of M(relids) */
static void
ec_count(PlannerInfo *root, RobustECGraph *g, Relids relids, int sign,
		 int *cnt)
{
	bool	   *active;
	Bitmapset  *x;
	ListCell   *lc;

	if (bms_is_empty(relids))
		return;
	x = ec_instances(root, relids);
	active = palloc0(sizeof(bool) * g->npairs);
	foreach(lc, g->stars)
		ec_forest((ECStar *) lfirst(lc), x, active);
	for (int p = 0; p < g->npairs; p++)
		if (active[p])
			cnt[p] += sign;
	pfree(active);
	bms_free(x);
}

/*
 * robust_ec_factor
 *		The factor of the multi-member equivalence classes,
 *		M(num[0]) M(num[1]) ... / (M(den[0]) M(den[1]) ...), as an "ec"
 *		querylet; NULL if it is 1 (no edge left after cancelling the edges
 *		common to the numerator and the denominator).  NULL relid sets are
 *		skipped.  The multiplier is the product of the remaining numerator
 *		edges' multipliers, then divided by the remaining denominator edges',
 *		each in pair order.
 */
RobustQuerylet *
robust_ec_factor(PlannerInfo *root, Relids *num, int nnum,
				 Relids *den, int nden, bool pushed)
{
	RobustECGraph *g = ec_graph(root);
	int		   *cnt;
	List	   *nl = NIL;
	List	   *dl = NIL;
	RobustQuerylet *q;
	ListCell   *lc;

	if (g == NULL || g->stars == NIL)
		return NULL;
	cnt = palloc0(sizeof(int) * g->npairs);
	for (int i = 0; i < nnum; i++)
		ec_count(root, g, num[i], 1, cnt);
	for (int i = 0; i < nden; i++)
		ec_count(root, g, den[i], -1, cnt);
	for (int p = 0; p < g->npairs; p++)
	{
		for (int c = 0; c < cnt[p]; c++)
			nl = lappend(nl, g->pairs[p]->q);
		for (int c = 0; c < -cnt[p]; c++)
			dl = lappend(dl, g->pairs[p]->q);
	}
	pfree(cnt);
	if (nl == NIL && dl == NIL)
		return NULL;

	q = palloc0(sizeof(RobustQuerylet));
	q->key = "ec";
	q->inst = "ec";
	q->sel_est = 0;
	q->bin = -1;
	q->pushed = pushed;
	q->ec_num = nl;
	q->ec_den = dl;
	q->mult = robust_vec_make(error_sample_count);
	for (int s = 0; s < q->mult->n; s++)
	{
		double		f = 1.0;

		foreach(lc, nl)
			f = f * ((RobustQuerylet *) lfirst(lc))->mult->v[s];
		foreach(lc, dl)
			f = f / ((RobustQuerylet *) lfirst(lc))->mult->v[s];
		q->mult->v[s] = f;
	}
	return q;
}

/* Is the instance pair (a, b), a < b, a pair of some multi-member class? */
static bool
ec_has_pair(PlannerInfo *root, Index a, Index b)
{
	RobustECGraph *g = ec_graph(root);

	return g != NULL && g->stars != NIL && ec_find_pair(g, a, b) >= 0;
}

/* The join clauses between two relation instances */
typedef struct ClauseGroup
{
	Index		a;				/* instance relids, a < b */
	Index		b;
	Relids		arels;			/* the base relids of a's side */
	Relids		brels;
	List	   *clauses;
	bool		pushed;			/* all clauses pushed down */
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
	return robust_join_querylets_at(root, clauses, NULL);
}

/*
 * robust_join_querylets_at
 *		robust_join_querylets() for the clauses applied at an outer join of
 *		joinrelids: a querylet all of whose clauses are pushed down (not the
 *		join's own ON clauses) is marked "pushed".  joinrelids NULL: none.
 */
List *
robust_join_querylets_at(PlannerInfo *root, List *clauses, Relids joinrelids)
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
		/* Multi-member classes enter through robust_ec_factor() */
		if (rinfo->parent_ec != NULL && robust_ec_is_star(root, rinfo->parent_ec))
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
			g->pushed = joinrelids != NULL;
			groups = lappend(groups, g);
		}
		if (joinrelids != NULL && !RINFO_IS_PUSHED_DOWN(rinfo, joinrelids))
			g->pushed = false;
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
		char	   *inst = pair_name(ia, ib);

		/*
		 * The pair is also an edge of a multi-member class: these other
		 * clauses draw independently (instance "ia=ib+"), so that the pair's
		 * draws are not counted twice when the edge is in a forest.
		 */
		if (ec_has_pair(root, g->a, g->b))
			inst = psprintf("%s+", inst);

		dummy_inner_sjinfo(&sjinfo, g->arels, g->brels);
		sel = clauselist_selectivity(root, g->clauses, 0, JOIN_INNER, &sjinfo);

		q = make_querylet(root, pair_name(a, b),
						  (sa && sb) ? pair_name(sa, sb) : NULL,
						  inst, sel);
		if (q != NULL)
		{
			q->pushed = g->pushed;
			q->ia = g->a;
			q->ib = g->b;
			result = lappend(result, q);
		}
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
		if (q->x == NULL)
		{
			/* an "ec" factor: the pair instances multiplied and divided */
			for (int side = 0; side < 2; side++)
			{
				List	   *l = side == 0 ? q->ec_num : q->ec_den;
				ListCell   *le;

				appendStringInfoString(&buf, side == 0 ? ",\"num\":[" : "],\"den\":[");
				foreach(le, l)
				{
					if (foreach_current_index(le) > 0)
						appendStringInfoChar(&buf, ',');
					escape_json(&buf, ((RobustQuerylet *) lfirst(le))->inst);
				}
			}
			appendStringInfoChar(&buf, ']');
		}
		else
		{
			appendStringInfoString(&buf, ",\"sel_est\":");
			robust_json_number(&buf, q->sel_est);
			appendStringInfo(&buf, ",\"bin\":%d", q->bin);
		}
		if (q->pushed)
			appendStringInfoString(&buf, ",\"pushed\":true");
		appendStringInfoChar(&buf, '}');
	}
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(d, "querylets", buf.data);
	pfree(buf.data);
}

/*
 * The "ecgraph" event: each multi-member class's members in rank order with
 * their numbers of profiled pairs, its profiled pairs in Kruskal order, and
 * the querylet of every member pair (key null: no profile).
 */
static void
dump_ec_graph(PlannerInfo *root, RobustECGraph *g)
{
	RobustDump	d;
	StringInfoData buf;
	ListCell   *lc;

	robust_dump_begin(&d, "ecgraph");
	robust_dump_str(&d, "pass", robust_pass_name(root->robust_pass));
	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	foreach(lc, g->stars)
	{
		ECStar	   *st = (ECStar *) lfirst(lc);

		if (foreach_current_index(lc) > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, "{\"members\":[");
		for (int i = 0; i < st->k; i++)
		{
			if (i > 0)
				appendStringInfoChar(&buf, ',');
			escape_json(&buf, st->name[i]);
		}
		appendStringInfoString(&buf, "],\"cover\":[");
		for (int i = 0; i < st->k; i++)
			appendStringInfo(&buf, "%s%d", i > 0 ? "," : "", st->cover[i]);
		appendStringInfoString(&buf, "],\"order\":[");
		for (int o = 0; o < st->norder; o++)
		{
			if (o > 0)
				appendStringInfoChar(&buf, ',');
			escape_json(&buf, g->pairs[st->order_pair[o]]->q->inst);
		}
		appendStringInfoString(&buf, "]}");
	}
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(&d, "ecs", buf.data);

	resetStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	for (int p = 0; p < g->npairs; p++)
	{
		ECPair	   *pr = g->pairs[p];
		char	   *inst = pair_name(robust_rel_name(root, pr->a),
									 robust_rel_name(root, pr->b));

		if (p > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, "{\"inst\":");
		escape_json(&buf, inst);
		appendStringInfo(&buf, ",\"nclauses\":%d", list_length(pr->clauses));
		if (pr->q != NULL)
		{
			appendStringInfoString(&buf, ",\"key\":");
			escape_json(&buf, pr->q->key);
			appendStringInfoString(&buf, ",\"sel_est\":");
			robust_json_number(&buf, pr->q->sel_est);
			appendStringInfo(&buf, ",\"bin\":%d", pr->q->bin);
		}
		else
			appendStringInfoString(&buf, ",\"key\":null");
		appendStringInfoChar(&buf, '}');
	}
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(&d, "pairs", buf.data);
	pfree(buf.data);
	robust_dump_end(&d);
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
	Relids		num = bms_union(rel->relids, ppi->ppi_req_outer);
	RobustQuerylet *ecq;

	/* Multi-member classes: M(rel + outer) / (M(rel) M(outer)), M(rel) = 1 */
	ecq = robust_ec_factor(root, &num, 1, &ppi->ppi_req_outer, 1, false);
	if (ecq != NULL)
		querylets = lappend(querylets, ecq);

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
		robust_dump_str(&d, "kind", "base");
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

/* ---------------------------------------------------------------- joins */

static const char *
jointype_name(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
			return "inner";
		case JOIN_LEFT:
			return "left";
		case JOIN_FULL:
			return "full";
		case JOIN_SEMI:
			return "semi";
		case JOIN_ANTI:
			return "anti";
		default:
			return "?";
	}
}

/*
 * join_rows_at
 *		calc_joinrel_size_estimate() in scenario s: input sizes o and i, and
 *		PostgreSQL's selectivities scaled by the querylets' multipliers.
 *
 * The multipliers of the join's own clauses scale fkselec * jselec (they
 * are folded into fkselec, so that every expression keeps the shape it has
 * in costsize.c and, with all multipliers 1, gives exactly PostgreSQL's
 * result, fused multiply-adds included).  Those of pushed-down clauses at
 * an outer join scale pselec.  A scaled selectivity is capped at 1:
 *
 *	inner	min(o * i * fkselec' * jselec, o * i)
 *	left	max(min(o * i * fkselec' * jselec, o * i), o) * pselec'
 *	full	max(min(o * i * fkselec' * jselec, o * i), o, i) * pselec'
 *	semi	min(o * fkselec' * jselec, o)
 *	anti	max(o * (1 - fkselec' * jselec), 0) * pselec'
 */
static double
join_rows_at(JoinType jointype, double o, double i, const RobustJoinSel *sel,
			 List *querylets, int s)
{
	double		mj = 1.0;
	double		mp = 1.0;
	double		fkm;
	double		ps;
	double		cap;
	double		n;
	ListCell   *lc;

	foreach(lc, querylets)
	{
		RobustQuerylet *q = (RobustQuerylet *) lfirst(lc);

		if (q->pushed)
			mp = mp * q->mult->v[s];
		else
			mj = mj * q->mult->v[s];
	}
	fkm = sel->fkselec * mj;
	ps = sel->pselec * mp;
	if (ps > 1.0)
		ps = 1.0;

	switch (jointype)
	{
		case JOIN_INNER:
			n = o * i * fkm * sel->jselec;
			cap = o * i;
			if (n > cap)
				n = cap;
			break;
		case JOIN_LEFT:
			n = o * i * fkm * sel->jselec;
			cap = o * i;
			if (n > cap)
				n = cap;
			if (n < o)
				n = o;
			n *= ps;
			break;
		case JOIN_FULL:
			n = o * i * fkm * sel->jselec;
			cap = o * i;
			if (n > cap)
				n = cap;
			if (n < o)
				n = o;
			if (n < i)
				n = i;
			n *= ps;
			break;
		case JOIN_SEMI:
			n = o * fkm * sel->jselec;
			if (n > o)
				n = o;
			break;
		case JOIN_ANTI:
			n = o * (1.0 - fkm * sel->jselec);
			if (n < 0.0)
				n = 0.0;
			n *= ps;
			break;
		default:
			elog(ERROR, "unrecognized join type: %d", (int) jointype);
			n = 0;				/* keep compiler quiet */
			break;
	}
	return clamp_row_est(n);
}

/*
 * The querylets of the clauses applied at a join, followed by the factor
 * of the multi-member equivalence classes, M(num...) / M(den...).  At an
 * outer join that factor scales pselec: an equivalence clause there is a
 * pushed-down qual, never the join's own ON clause.
 */
static List *
join_querylets(PlannerInfo *root, RelOptInfo *joinrel, SpecialJoinInfo *sjinfo,
			   List *clauses, Relids *num, int nnum, Relids *den, int nden)
{
	List	   *result;
	RobustQuerylet *ecq;

	result = robust_join_querylets_at(root, clauses,
									  IS_OUTER_JOIN(sjinfo->jointype) ?
									  joinrel->relids : NULL);
	ecq = robust_ec_factor(root, num, nnum, den, nden,
						   IS_OUTER_JOIN(sjinfo->jointype));
	if (ecq != NULL)
		result = lappend(result, ecq);
	return result;
}

/*
 * robust_join_querylets_ec
 *		The querylets of the clauses applied at a join of joinrel described by
 *		sjinfo, followed by the factor M(num...) / M(den...) of the
 *		multi-member equivalence classes (as for the join's row count).
 */
List *
robust_join_querylets_ec(PlannerInfo *root, RelOptInfo *joinrel,
						 SpecialJoinInfo *sjinfo, List *clauses,
						 Relids *num, int nnum, Relids *den, int nden)
{
	return join_querylets(root, joinrel, sjinfo, clauses, num, nnum, den, nden);
}

static void
dump_join_common(RobustDump *d, PlannerInfo *root, SpecialJoinInfo *sjinfo,
				 Relids outer, Relids inner, const RobustJoinSel *sel,
				 List *querylets, const RobustVec *orows, const RobustVec *irows)
{
	StringInfoData buf;

	robust_dump_str(d, "jointype", jointype_name(sjinfo->jointype));
	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	escape_json(&buf, robust_relids_name(root, outer));
	appendStringInfoChar(&buf, ',');
	escape_json(&buf, robust_relids_name(root, inner));
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(d, "pair", buf.data);
	resetStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	robust_json_number(&buf, sel->fkselec);
	appendStringInfoChar(&buf, ',');
	robust_json_number(&buf, sel->jselec);
	appendStringInfoChar(&buf, ',');
	robust_json_number(&buf, sel->pselec);
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(d, "sel", buf.data);
	pfree(buf.data);
	dump_querylets(d, querylets);
	robust_dump_vec(d, "outer_rows", orows);
	robust_dump_vec(d, "inner_rows", irows);
}

/*
 * Semi and anti joins: replaying the selectivity estimate per scenario.
 *
 * PostgreSQL's estimate of the fraction of outer rows that have a match
 * (eqjoinsel_semi) depends on the size of the inner side: the number of
 * distinct inner values is capped by the inner relation's rows and by the
 * rows of the base relation the inner variable comes from, and the fraction
 * is capped by inner rows times the inner-join selectivity.  So for these
 * joins the selectivities are estimated again in each scenario, with the
 * rows fields of the joined relations and of their base relations set to
 * the scenario's values (and fresh copies of the clauses, so that the
 * selectivities cached in the RestrictInfos are neither used nor changed).
 * The selectivities of inner and outer joins do not depend on the input
 * rows, so they are estimated once.
 */
typedef struct ReplayRel
{
	RelOptInfo *rel;
	RobustVec  *rows;
	double		saved;
} ReplayRel;

/* The relations whose rows the estimate may read, and their vectors */
static List *
replay_rels(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outer_rel,
			RelOptInfo *inner_rel, bool *varies)
{
	List	   *rels = NIL;
	int			relid = -1;

	*varies = false;
	while ((relid = bms_next_member(joinrel->relids, relid)) >= 0)
	{
		RelOptInfo *rel;
		ReplayRel  *r;

		if (relid >= root->simple_rel_array_size ||
			(rel = root->simple_rel_array[relid]) == NULL)
			continue;
		r = palloc(sizeof(ReplayRel));
		r->rel = rel;
		r->rows = robust_rel_rows(rel);
		*varies |= rel->robust_rows != NULL;
		rels = lappend(rels, r);
	}
	for (int side = 0; side < 2; side++)
	{
		RelOptInfo *rel = side == 0 ? outer_rel : inner_rel;
		ReplayRel  *r;

		if (rel->reloptkind != RELOPT_JOINREL && rel->reloptkind != RELOPT_OTHER_JOINREL)
			continue;			/* a base relation, already listed */
		r = palloc(sizeof(ReplayRel));
		r->rel = rel;
		r->rows = robust_rel_rows(rel);
		*varies |= rel->robust_rows != NULL;
		rels = lappend(rels, r);
	}
	return rels;
}

static void
replay_sel(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outer_rel,
		   RelOptInfo *inner_rel, SpecialJoinInfo *sjinfo, List *restrictlist,
		   List *rels, int s, RobustJoinSel *out)
{
	List	   *copies = NIL;
	ListCell   *lc;

	foreach(lc, restrictlist)
	{
		RestrictInfo *c = makeNode(RestrictInfo);

		memcpy(c, lfirst_node(RestrictInfo, lc), sizeof(RestrictInfo));
		c->norm_selec = -1;
		c->outer_selec = -1;
		copies = lappend(copies, c);
	}
	foreach(lc, rels)
	{
		ReplayRel  *r = (ReplayRel *) lfirst(lc);

		r->saved = r->rel->rows;
		r->rel->rows = robust_vec_at(r->rows, s);
	}
	(void) robust_calc_joinrel_size(root, joinrel, outer_rel, inner_rel,
									outer_rel->rows, inner_rel->rows,
									sjinfo, copies, out);
	foreach(lc, rels)
	{
		ReplayRel  *r = (ReplayRel *) lfirst(lc);

		r->rel->rows = r->saved;
	}
	list_free_deep(copies);
}

/*
 * The join rows of every scenario; NULL if they are all the point estimate.
 * sels (if not NULL) receives the replayed selectivities, NULL if none.
 */
static RobustVec *
join_rows(PlannerInfo *root, RelOptInfo *joinrel, RelOptInfo *outer_rel,
		  RelOptInfo *inner_rel, SpecialJoinInfo *sjinfo, List *restrictlist,
		  const RobustJoinSel *sel, List *querylets,
		  const RobustVec *orows, const RobustVec *irows,
		  const RobustVec *cap, bool inputs_vary, RobustJoinSel **sels)
{
	bool		semi = sjinfo->jointype == JOIN_SEMI || sjinfo->jointype == JOIN_ANTI;
	bool		replay_varies = false;
	List	   *rels = NIL;
	RobustVec  *rows;

	*sels = NULL;
	if (semi)
		rels = replay_rels(root, joinrel, outer_rel, inner_rel, &replay_varies);
	if (querylets == NIL && !inputs_vary && !replay_varies)
		return NULL;

	rows = robust_vec_make(error_sample_count);
	if (replay_varies)
		*sels = palloc(sizeof(RobustJoinSel) * rows->n);
	for (int s = 0; s < rows->n; s++)
	{
		const RobustJoinSel *ss = sel;
		double		n;

		if (replay_varies)
		{
			replay_sel(root, joinrel, outer_rel, inner_rel, sjinfo,
					   restrictlist, rels, s, &(*sels)[s]);
			ss = &(*sels)[s];
		}
		n = join_rows_at(sjinfo->jointype, robust_vec_at(orows, s),
						 robust_vec_at(irows, s), ss, querylets, s);
		if (cap != NULL && n > robust_vec_at(cap, s))
			n = robust_vec_at(cap, s);
		rows->v[s] = n;
	}
	return rows;
}

static void
dump_sels(RobustDump *d, const RobustJoinSel *sels, int n)
{
	StringInfoData buf;

	if (sels == NULL)
		return;
	initStringInfo(&buf);
	appendStringInfoChar(&buf, '[');
	for (int s = 0; s < n; s++)
	{
		if (s > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfoChar(&buf, '[');
		robust_json_number(&buf, sels[s].fkselec);
		appendStringInfoChar(&buf, ',');
		robust_json_number(&buf, sels[s].jselec);
		appendStringInfoChar(&buf, ',');
		robust_json_number(&buf, sels[s].pselec);
		appendStringInfoChar(&buf, ']');
	}
	appendStringInfoChar(&buf, ']');
	robust_dump_raw(d, "sels", buf.data);
	pfree(buf.data);
}

/*
 * robust_set_joinrel_rows
 *		The row vector of a join relation, built from the pair outer_rel /
 *		inner_rel, whose selectivities PostgreSQL estimated as *sel.
 *
 * Like rel->rows, the vector is computed from the first pair that builds
 * the join relation (store = true, from set_joinrel_size_estimates).  With
 * the debug dump on, build_join_rel() also calls this for every later pair
 * (store = false, sel = NULL: the selectivities are estimated here), which
 * only reports the vector that pair would give.
 */
void
robust_set_joinrel_rows(PlannerInfo *root, RelOptInfo *joinrel,
						RelOptInfo *outer_rel, RelOptInfo *inner_rel,
						SpecialJoinInfo *sjinfo, List *restrictlist,
						const RobustJoinSel *sel, bool store)
{
	RobustJoinSel local;
	double		pair_point;
	RobustVec  *orows = robust_rel_rows(outer_rel);
	RobustVec  *irows = robust_rel_rows(inner_rel);
	RobustVec  *rows = NULL;
	RobustJoinSel *sels;
	List	   *querylets;

	if (sel == NULL)
	{
		pair_point = robust_calc_joinrel_size(root, joinrel, outer_rel,
											  inner_rel, outer_rel->rows,
											  inner_rel->rows, sjinfo,
											  restrictlist, &local);
		sel = &local;
	}
	else
		pair_point = joinrel->rows;

	{
		/* M(joinrel) / (M(outer) M(inner)) */
		Relids		num = joinrel->relids;
		Relids		den[2] = {outer_rel->relids, inner_rel->relids};

		querylets = join_querylets(root, joinrel, sjinfo, restrictlist,
								   &num, 1, den, 2);
	}
	rows = join_rows(root, joinrel, outer_rel, inner_rel, sjinfo, restrictlist,
					 sel, querylets, orows, irows, NULL,
					 outer_rel->robust_rows != NULL ||
					 inner_rel->robust_rows != NULL, &sels);
	if (store)
	{
		joinrel->robust_rows = rows;
		joinrel->robust_querylets = querylets;
	}

	if (robust_debug_dump)
	{
		RobustDump	d;

		robust_dump_begin(&d, "rel");
		robust_dump_str(&d, "pass", robust_pass_name(root->robust_pass));
		robust_dump_relids(&d, "rels", root, joinrel->relids);
		robust_dump_str(&d, "kind", "join");
		robust_dump_bool(&d, "first", store);
		dump_join_common(&d, root, sjinfo, outer_rel->relids, inner_rel->relids,
						 sel, querylets, orows, irows);
		dump_sels(&d, sels, error_sample_count);
		robust_dump_double(&d, "rows_point", joinrel->rows);
		robust_dump_double(&d, "pair_point", pair_point);
		robust_dump_vec(&d, "rows", rows ? rows : robust_vec_const(pair_point));
		robust_dump_end(&d);
	}
}

/*
 * robust_set_joinrel_ppi_rows
 *		The row vector of a parameterized join, from the pair of paths that
 *		first needed the ParamPathInfo, as get_parameterized_joinrel_size()
 *		computes ppi_rows: the join formula on the paths' rows, capped by the
 *		join relation's rows.
 */
void
robust_set_joinrel_ppi_rows(PlannerInfo *root, RelOptInfo *joinrel,
							ParamPathInfo *ppi, Path *outer_path,
							Path *inner_path, SpecialJoinInfo *sjinfo,
							List *restrict_clauses, const RobustJoinSel *sel)
{
	const char *osrc;
	const char *isrc;
	bool		ovaries;
	bool		ivaries;
	RobustVec  *orows = robust_path_rows(outer_path, &osrc, &ovaries);
	RobustVec  *irows = robust_path_rows(inner_path, &isrc, &ivaries);
	RobustVec  *relrows = robust_rel_rows(joinrel);
	RobustVec  *rows;
	RobustJoinSel *sels;
	List	   *querylets;
	Relids		req = ppi->ppi_req_outer;
	Relids		oreq = PATH_REQ_OUTER(outer_path);
	Relids		ireq = PATH_REQ_OUTER(inner_path);

	/*
	 * A relation X parameterized by R carries the class factor
	 * F(X | R) = M(X + R) / M(R) (R = {} for an unparameterized one).  The
	 * input paths carry F(outer | oreq) and F(inner | ireq), and the result
	 * must carry F(join | req), so the factor applied here is
	 *
	 *	M(join + req) M(oreq) M(ireq) / (M(req) M(outer + oreq) M(inner + ireq))
	 */
	{
		Relids		num[3] = {bms_union(joinrel->relids, req), oreq, ireq};
		Relids		den[3] = {req,
			bms_union(outer_path->parent->relids, oreq),
		bms_union(inner_path->parent->relids, ireq)};

		querylets = join_querylets(root, joinrel, sjinfo, restrict_clauses,
								   num, 3, den, 3);
	}

	rows = join_rows(root, joinrel, outer_path->parent, inner_path->parent,
					 sjinfo, restrict_clauses, sel, querylets, orows, irows,
					 relrows,
					 ovaries || ivaries || joinrel->robust_rows != NULL,
					 &sels);
	ppi->ppi_robust_rows = rows;
	ppi->ppi_robust_querylets = querylets;

	if (robust_debug_dump)
	{
		RobustDump	d;
		StringInfoData buf;
		char	   *oname = robust_relids_name(root, PATH_REQ_OUTER(outer_path));
		char	   *iname = robust_relids_name(root, PATH_REQ_OUTER(inner_path));

		robust_dump_begin(&d, "ppi");
		robust_dump_relids(&d, "rels", root, joinrel->relids);
		robust_dump_relids(&d, "outer", root, ppi->ppi_req_outer);
		robust_dump_str(&d, "kind", "join");
		dump_join_common(&d, root, sjinfo, outer_path->parent->relids,
						 inner_path->parent->relids, sel, querylets,
						 orows, irows);
		initStringInfo(&buf);
		appendStringInfoChar(&buf, '[');
		if (oname)
			escape_json(&buf, oname);
		else
			appendStringInfoString(&buf, "null");
		appendStringInfoChar(&buf, ',');
		if (iname)
			escape_json(&buf, iname);
		else
			appendStringInfoString(&buf, "null");
		appendStringInfoChar(&buf, ']');
		robust_dump_raw(&d, "pair_param", buf.data);
		resetStringInfo(&buf);
		appendStringInfo(&buf, "[\"%s\",\"%s\"]", osrc, isrc);
		robust_dump_raw(&d, "pair_src", buf.data);
		pfree(buf.data);
		dump_sels(&d, sels, error_sample_count);
		robust_dump_double(&d, "rows_point", ppi->ppi_rows);
		robust_dump_vec(&d, "rows", rows ? rows : robust_vec_const(ppi->ppi_rows));
		robust_dump_end(&d);
	}
}
