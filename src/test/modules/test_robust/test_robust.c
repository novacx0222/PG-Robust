/*--------------------------------------------------------------------------
 *
 * test_robust.c
 *		Unit tests for the RobDP robust planning support code.
 *
 * test_robustvec() exercises the RobustVec API and returns "ok" or raises
 * an error naming the first failed check.  test_robust_dump() emits one
 * debug line with awkward values, so that the expected output pins down
 * the exact dump format.  test_robust_profile() and test_robust_draw()
 * expose profile lookup and error drawing to SQL.
 *
 * src/test/modules/test_robust/test_robust.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "optimizer/optimizer.h"
#include "optimizer/robust.h"
#include "optimizer/robustprofile.h"
#include "utils/array.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_robustvec);
PG_FUNCTION_INFO_V1(test_robust_dump);
PG_FUNCTION_INFO_V1(test_robust_profile);
PG_FUNCTION_INFO_V1(test_robust_draw);

#define CHECK(cond) \
	do { \
		if (!(cond)) \
			elog(ERROR, "test_robustvec: check failed at line %d: %s", \
				 __LINE__, #cond); \
	} while (0)

/* Does fn(arg) raise an error?  The error is swallowed. */
static bool
raises_error(void (*fn) (void *), void *arg)
{
	MemoryContext oldcxt = CurrentMemoryContext;
	bool		raised = false;

	PG_TRY();
	{
		fn(arg);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcxt);
		FlushErrorState();
		raised = true;
	}
	PG_END_TRY();
	return raised;
}

static void
call_make(void *arg)
{
	(void) robust_vec_make(*(int *) arg);
}

typedef struct CheckArgs
{
	const RobustVec *v;
	int			n;
} CheckArgs;

static void
call_check(void *arg)
{
	CheckArgs  *a = (CheckArgs *) arg;

	robust_vec_check(a->v, a->n);
}

Datum
test_robustvec(PG_FUNCTION_ARGS)
{
	RobustVec  *v;
	RobustVec  *c;
	RobustVec  *w;
	int			len;
	CheckArgs	ca;
	static const double raw[] = {-5.0, 0.0, 0.4, 1.0, 1.5, 2.5, 3.5, 1e300,
	NAN, INFINITY};
	const int	nraw = lengthof(raw);

	/* make: zeroed, right length; invalid lengths are errors */
	v = robust_vec_make(3);
	CHECK(v->n == 3);
	CHECK(v->v[0] == 0.0 && v->v[1] == 0.0 && v->v[2] == 0.0);
	len = 0;
	CHECK(raises_error(call_make, &len));
	len = ROBUST_MAX_SAMPLES + 1;
	CHECK(raises_error(call_make, &len));
	len = ROBUST_MAX_SAMPLES;
	CHECK(!raises_error(call_make, &len));

	/* const: same value at every sample */
	c = robust_vec_const(5.25);
	CHECK(c->n == 1);
	for (int s = 0; s < ROBUST_MAX_SAMPLES; s++)
		CHECK(robust_vec_at(c, s) == 5.25);

	/* at: component s of a full vector */
	for (int i = 0; i < 3; i++)
		v->v[i] = 10.0 * (i + 1);
	for (int s = 0; s < 3; s++)
		CHECK(robust_vec_at(v, s) == 10.0 * (s + 1));

	/* copy: equal and independent; NULL stays NULL */
	w = robust_vec_copy(v);
	CHECK(w != v && w->n == 3 && memcmp(w->v, v->v, 3 * sizeof(double)) == 0);
	w->v[0] = -1.0;
	CHECK(v->v[0] == 10.0);
	CHECK(robust_vec_copy(NULL) == NULL);

	/* check: length 1 or N only, never NULL */
	ca.v = c;
	ca.n = 20;
	CHECK(!raises_error(call_check, &ca));
	ca.v = v;
	ca.n = 3;
	CHECK(!raises_error(call_check, &ca));
	ca.n = 20;
	CHECK(raises_error(call_check, &ca));
	ca.v = NULL;
	CHECK(raises_error(call_check, &ca));

	/* varies: a full vector of equal components does not vary */
	CHECK(!robust_vec_varies(c));
	CHECK(robust_vec_varies(v));
	w = robust_vec_make(4);
	for (int i = 0; i < 4; i++)
		w->v[i] = 7.0;
	CHECK(!robust_vec_varies(w));
	w->v[3] = 7.0000000001;
	CHECK(robust_vec_varies(w));

	/* mean */
	CHECK(robust_vec_mean(v) == 20.0);
	CHECK(robust_vec_mean(c) == 5.25);

	/* clamp_rows: bit-identical to clamp_row_est() on every component */
	w = robust_vec_make(nraw);
	memcpy(w->v, raw, sizeof(raw));
	robust_vec_clamp_rows(w);
	for (int i = 0; i < nraw; i++)
	{
		double		expected = clamp_row_est(raw[i]);

		CHECK(memcmp(&w->v[i], &expected, sizeof(double)) == 0);
	}
	CHECK(w->v[0] == 1.0 && w->v[2] == 1.0 && w->v[4] == 2.0);
	CHECK(w->v[5] == 2.0 && w->v[6] == 4.0);	/* rint: round half to even */

	PG_RETURN_TEXT_P(cstring_to_text("ok"));
}

Datum
test_robust_dump(PG_FUNCTION_ARGS)
{
	RobustDump	d;
	RobustVec  *v = robust_vec_make(6);

	v->v[0] = 0.1;
	v->v[1] = 1.0 / 3.0;
	v->v[2] = 1e-300;
	v->v[3] = NAN;
	v->v[4] = INFINITY;
	v->v[5] = -0.0;

	robust_dump_begin(&d, "test");
	robust_dump_str(&d, "s", "quote\" backslash\\ tab\t é");
	robust_dump_str(&d, "null_s", NULL);
	robust_dump_int(&d, "i", PG_INT64_MIN);
	robust_dump_double(&d, "big", 123456789012345680.0);
	robust_dump_double(&d, "ninf", -INFINITY);
	robust_dump_bool(&d, "t", true);
	robust_dump_bool(&d, "f", false);
	robust_dump_vec(&d, "v", v);
	robust_dump_vec(&d, "c", robust_vec_const(2.0));
	robust_dump_vec(&d, "null_v", NULL);
	robust_dump_end(&d);

	PG_RETURN_VOID();
}

Datum
test_robust_profile(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(1);
	const RobustProfile *prof;
	Datum	   *elems;
	bool	   *nulls;
	int			n;
	Datum	   *bins;

	prof = robust_get_profile(key);
	if (prof == NULL)
		PG_RETURN_NULL();

	deconstruct_array_builtin(arr, FLOAT8OID, &elems, &nulls, &n);
	bins = palloc(sizeof(Datum) * (n > 0 ? n : 1));
	for (int i = 0; i < n; i++)
	{
		if (nulls[i])
			elog(ERROR, "test_robust_profile: null estimate");
		bins[i] = Int32GetDatum(robust_profile_find_bin(prof, DatumGetFloat8(elems[i])));
	}
	PG_RETURN_ARRAYTYPE_P(construct_array_builtin(bins, n, INT4OID));
}

Datum
test_robust_draw(PG_FUNCTION_ARGS)
{
	char	   *key = text_to_cstring(PG_GETARG_TEXT_PP(0));
	char	   *inst = text_to_cstring(PG_GETARG_TEXT_PP(1));
	double		sel_est = PG_GETARG_FLOAT8(2);
	const RobustProfile *prof;
	RobustVec  *x;
	Datum	   *vals;

	prof = robust_get_profile(key);
	if (prof == NULL)
		PG_RETURN_NULL();
	x = robust_draw_errors(prof, inst, sel_est, NULL);
	if (x == NULL)
		PG_RETURN_NULL();

	vals = palloc(sizeof(Datum) * x->n);
	for (int s = 0; s < x->n; s++)
		vals[s] = Float8GetDatum(x->v[s]);
	PG_RETURN_ARRAYTYPE_P(construct_array(vals, x->n, FLOAT8OID, sizeof(float8),
										  FLOAT8PASSBYVAL, TYPALIGN_DOUBLE));
}
