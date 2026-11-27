/*-------------------------------------------------------------------------
 *
 * robust.h
 *	  Settings, planner state and debug output of RobDP robust planning.
 *
 * RobDP plans a query against a sample of possible true selectivities
 * instead of PostgreSQL's point estimates.  With enable_rows_dist off (the
 * default) none of it runs and the planner is PostgreSQL's.
 *
 * src/include/optimizer/robust.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ROBUST_H
#define ROBUST_H

#include "lib/stringinfo.h"
#include "nodes/pathnodes.h"
#include "optimizer/robustvec.h"
#include "utils/guc.h"

/* GUC parameters */
extern PGDLLIMPORT bool enable_rows_dist;
extern PGDLLIMPORT int error_sample_count;
extern PGDLLIMPORT int error_sample_seed;
extern PGDLLIMPORT int error_bin_count;
extern PGDLLIMPORT double error_sample_kde_bandwidth;
extern PGDLLIMPORT char *error_profile_path;
extern PGDLLIMPORT bool robust_debug_dump;
extern PGDLLIMPORT bool robust_search;
extern PGDLLIMPORT bool robust_envelope;
extern PGDLLIMPORT bool robust_select_dedup;
extern PGDLLIMPORT bool robust_select_precheck;
extern PGDLLIMPORT bool robust_vector_cost;
extern PGDLLIMPORT bool robust_projected_rounds;
extern PGDLLIMPORT bool robust_ec_forest;
extern PGDLLIMPORT bool robust_debug_check_costs;
extern PGDLLIMPORT bool robust_gather_all;
extern PGDLLIMPORT bool robust_select;
extern PGDLLIMPORT bool robust_dominance;
extern PGDLLIMPORT int robust_join_pairing;
extern PGDLLIMPORT int add_path_limit;
extern PGDLLIMPORT int retain_path_limit;
extern PGDLLIMPORT int robust_final_path_limit;
extern PGDLLIMPORT int main_objective_id;
extern PGDLLIMPORT int retain_strategy_id;
extern PGDLLIMPORT double robust_penalty_tau;
extern PGDLLIMPORT double robust_penalty_lambda;
extern PGDLLIMPORT double robust_quantile_alpha;

/* robust_debug_check_costs is on by default in assert-enabled builds */
#ifdef USE_ASSERT_CHECKING
#define DEFAULT_ROBUST_CHECK_COSTS true
#else
#define DEFAULT_ROBUST_CHECK_COSTS false
#endif

/* robust_join_pairing (see optimizer/robustselect.h) */
typedef enum RobustPairing
{
	ROBUST_PAIRING_NATIVE,		/* PostgreSQL's input paths */
	ROBUST_PAIRING_RETAINED,	/* every path the inputs keep */
} RobustPairing;

/*
 * The objectives and diversification functions, v20's numbering
 * (main_objective_id: an objective; retain_strategy_id: either; see
 * optimizer/robustselect.h)
 */
#define ROBUST_OBJECTIVE_MAX	18

extern bool check_robust_objective(int *newval, void **extra, GucSource source);
extern bool check_robust_strategy(int *newval, void **extra, GucSource source);

/* Upper limit of error_bin_count */
#define ROBUST_MAX_BINS		8

/*
 * The phase of robust planning, PlannerInfo.robust_pass.
 *
 * ROBUST_PASS_NONE (zero, so a new PlannerInfo starts there) means robust
 * planning is not running for this query level, or we are outside the
 * parts it changes.  The other phases are those of the paper's
 * Algorithm 1 as built on PostgreSQL's planner:
 *
 *	BASE		base relations get their per-sample row counts and paths
 *	ENVELOPE	optional DP pass, once per sample (PlannerInfo.robust_round),
 *				recording the optimal cost of each join relation
 *	MAIN		the main DP pass, costing every path at every sample
 *	UPPER		planning above the join tree (grouping, sorting, LIMIT)
 */
typedef enum RobustPass
{
	ROBUST_PASS_NONE = 0,
	ROBUST_PASS_BASE,
	ROBUST_PASS_ENVELOPE,
	ROBUST_PASS_MAIN,
	ROBUST_PASS_UPPER,
} RobustPass;

extern const char *robust_pass_name(RobustPass pass);

/* Is the debug dump on for this query level?  Not in the envelope pass. */
#define robust_dumping(root) \
	(robust_debug_dump && (root)->robust_pass != ROBUST_PASS_ENVELOPE)

/*
 * Debug output (robust_debug_dump): one LOG line "ROBDP {json}" per event,
 * for the test harness.  The format is defined in
 * src/tools/robdp/ROBDP_DUMP_SPEC.md.  Usage:
 *
 *		RobustDump	d;
 *
 *		robust_dump_begin(&d, "rel");
 *		robust_dump_int(&d, "n", 3);
 *		robust_dump_vec(&d, "rows", rows);
 *		robust_dump_end(&d);
 */
typedef struct RobustDump
{
	StringInfoData buf;
} RobustDump;

extern void robust_dump_begin(RobustDump *d, const char *ev);
extern void robust_dump_str(RobustDump *d, const char *key, const char *val);
extern void robust_dump_int(RobustDump *d, const char *key, int64 val);
extern void robust_dump_double(RobustDump *d, const char *key, double val);
extern void robust_dump_bool(RobustDump *d, const char *key, bool val);
extern void robust_dump_raw(RobustDump *d, const char *key, const char *json);
extern void robust_dump_vec(RobustDump *d, const char *key, const RobustVec *v);
extern void robust_dump_relids(RobustDump *d, const char *key,
							   PlannerInfo *root, Relids relids);
extern Index robust_instance_relid(PlannerInfo *root, Index relid);
extern char *robust_rel_name(PlannerInfo *root, Index relid);
extern char *robust_relids_name(PlannerInfo *root, Relids relids);
extern void robust_dump_end(RobustDump *d);
extern void robust_json_number(StringInfo buf, double val);

extern void robust_dump_query(void);

#endif							/* ROBUST_H */
