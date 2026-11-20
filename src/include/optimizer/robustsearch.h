/*-------------------------------------------------------------------------
 *
 * robustsearch.h
 *	  The join search of RobDP: envelope pass and main pass.
 *
 * robust_join_search() (allpaths.c) replaces standard_join_search() when
 * enable_rows_dist and robust_search are on:
 *
 *	ENVELOPE	(robust_envelope) for each scenario s = 0 .. N-1, a round of
 *				PostgreSQL's join search in which every relation has its rows
 *				in s and every path its costs in s, in the scalar fields: the
 *				level-1 relations are projected onto s ("round view"), the
 *				join relations and ParamPathInfos built in the round take
 *				their rows in s (robustrows.c), and each path built is
 *				replayed in s (robustcost.c).  The cheapest total cost of
 *				each relation U in round s is C*(U, s), the envelope.  The
 *				round's relations and paths are then thrown away, as GEQO
 *				does for each join order it tries.
 *	MAIN		PostgreSQL's join search once more, every path with its
 *				vectors.  With robust_select on, add_path() collects every
 *				candidate and each relation, once complete, keeps the paths
 *				its objectives select (optimizer/robustselect.h); with it
 *				off, add_path() keeps the paths no other path dominates in
 *				every scenario (pathnode.c).
 *
 * The envelope has two parts per relation: C*(U, s) of its complete paths
 * (Gather paths included) and of its partial paths, whose costs are per
 * worker.  It is recorded for the child joins of partitionwise joins too.
 * A selecting search runs the envelope pass only if its objectives need it.
 *
 * Planning caches its estimates in long-lived structures that a round must
 * not leave behind: the selectivities and hash bucket statistics in the
 * RestrictInfos (which depend on input row counts for semi/anti joins and
 * hash joins), put back to their values before the search at the start of
 * each round and of the main pass; the unique-ified paths of the level-1
 * relations; and robustcost.c's own state.
 *
 * src/include/optimizer/robustsearch.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ROBUSTSEARCH_H
#define ROBUSTSEARCH_H

#include "nodes/pathnodes.h"

typedef struct RobustSearch RobustSearch;

extern RobustSearch *robust_search_begin(PlannerInfo *root, List *initial_rels);
extern bool robust_search_rounds(RobustSearch *rs);
extern void robust_round_begin(RobustSearch *rs, int s);
extern void robust_round_record(RobustSearch *rs, RelOptInfo *rel);
extern void robust_round_end(RobustSearch *rs);
extern void robust_main_begin(RobustSearch *rs);
extern void robust_search_finish_rel(RobustSearch *rs, RelOptInfo *rel,
									 bool top);
extern void robust_search_child_rel(RobustSearch *rs, RelOptInfo *child);
extern void robust_dump_pathlist(PlannerInfo *root, RelOptInfo *rel);
extern void robust_search_end(RobustSearch *rs);

#endif							/* ROBUSTSEARCH_H */
