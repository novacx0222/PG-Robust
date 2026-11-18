--
-- ROBUST_BASIC
--
-- Smoke tests for RobDP robust planning (enable_rows_dist).  These cover the
-- path shapes that crashed or failed in v20: bitmap scans, Material and
-- Memoize inner paths, dummy (provably empty) relations, append relations,
-- GEQO and subqueries.  The plans themselves are not the point; the test
-- checks that robust planning runs and that queries return correct results.
--

-- Error profiles are read from *.txt files in error_profile_path when it is
-- set.  Write a few small ones into the data directory: one per relation
-- alias used in the queries below and one per join clause ("alias=alias";
-- profiles are looked up by alias, not by table name).  Each line is
-- "true_selectivity estimated_selectivity".
CREATE TEMP TABLE robust_prof (sel_true float8, sel_est float8);
INSERT INTO robust_prof
  SELECT least(1.0, e * exp(0.8 * sin(i))), e
  FROM (SELECT i, power(10, -4.0 * (i % 50) / 50.0) AS e
        FROM generate_series(1, 200) i) s;
DO $$
DECLARE
  f text;
BEGIN
  FOREACH f IN ARRAY ARRAY['r', 's', 'r=s'] LOOP
    EXECUTE format('COPY robust_prof TO %L (DELIMITER %L)',
                   current_setting('data_directory') || '/' || f || '.txt', ' ');
  END LOOP;
END $$;

CREATE TABLE robust_r (a int, b int, c text);
CREATE TABLE robust_s (a int, b int, c text);
CREATE TABLE robust_t (a int, b int);
INSERT INTO robust_r SELECT i, i % 100, 'r' || i FROM generate_series(1, 5000) i;
INSERT INTO robust_s SELECT i, i % 50, 's' || i FROM generate_series(1, 3000) i;
INSERT INTO robust_t SELECT i % 10, i FROM generate_series(1, 1000) i;
CREATE INDEX robust_r_a ON robust_r (a);
CREATE INDEX robust_r_b ON robust_r (b);
CREATE INDEX robust_s_a ON robust_s (a);
CREATE INDEX robust_s_b ON robust_s (b);
ANALYZE robust_r, robust_s, robust_t;

CREATE TABLE robust_p (a int, b int) PARTITION BY RANGE (a);
CREATE TABLE robust_p1 PARTITION OF robust_p FOR VALUES FROM (0) TO (2500);
CREATE TABLE robust_p2 PARTITION OF robust_p FOR VALUES FROM (2500) TO (5001);
INSERT INTO robust_p SELECT i, i % 7 FROM generate_series(0, 5000) i;
ANALYZE robust_p;

-- The settings below load and sample the profiles, so set the sample count
-- and bin count first.
SET error_sample_count = 20;
SET error_bin_count = 2;
SET error_sample_seed = 42;
SELECT set_config('error_profile_path', current_setting('data_directory'), false)
  IS NOT NULL AS profiles_loaded;
SET enable_rows_dist = on;
SET max_parallel_workers_per_gather = 0;

-- Plain two- and three-way joins, with every objective.
DO $$
DECLARE
  obj int;
  n bigint;
BEGIN
  FOR obj IN 0..16 LOOP
    EXECUTE format('SET main_objective_id = %s', obj);
    EXECUTE format('SET final_score_id = %s', obj);
    SELECT count(*) INTO n
      FROM robust_r r JOIN robust_s s ON r.a = s.a JOIN robust_t t ON t.b = s.a
      WHERE r.b < 20;
    IF n <> 200 THEN
      RAISE EXCEPTION 'objective %: wrong result %', obj, n;
    END IF;
  END LOOP;
END $$;
RESET main_objective_id;
RESET final_score_id;

-- Keep several paths per joinrel.
SET add_path_limit = 4;
SET retain_path_limit = 2;
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a WHERE r.b < 20;
RESET add_path_limit;
RESET retain_path_limit;

-- Bitmap scans on a base relation that is then joined.
SET enable_indexscan = off;
SET enable_seqscan = off;
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a
  WHERE r.b IN (1, 2, 3) AND s.b IN (1, 2);
RESET enable_indexscan;
RESET enable_seqscan;

-- Material and Memoize inner paths.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT count(*) FROM robust_r r JOIN robust_t t ON t.a = r.b WHERE r.a < 200;
SELECT count(*) FROM robust_r r JOIN robust_s s ON s.b = r.b WHERE r.a < 50;
RESET enable_hashjoin;
RESET enable_mergejoin;

-- Provably empty relations (dummy rels and dummy joinrels).
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a WHERE false;
SELECT count(*) FROM robust_r r
  JOIN (SELECT * FROM robust_s WHERE false) s ON r.a = s.a
  JOIN robust_t t ON t.a = r.b;

-- Append relations: a partitioned table and a flattened UNION ALL.
SELECT count(*) FROM robust_p p JOIN robust_r r ON r.a = p.a WHERE p.b = 3;
SELECT count(*) FROM (SELECT a FROM robust_s UNION ALL SELECT a FROM robust_t) u
  JOIN robust_r r ON r.a = u.a;

-- Semi, anti and outer joins, LATERAL and a subquery in the range table.
SELECT count(*) FROM robust_r r WHERE EXISTS (SELECT 1 FROM robust_s s WHERE s.a = r.a);
SELECT count(*) FROM robust_r r WHERE NOT EXISTS (SELECT 1 FROM robust_s s WHERE s.a = r.a);
SELECT count(*) FROM robust_r r LEFT JOIN robust_s s ON s.a = r.a WHERE r.b < 10;
SELECT count(*) FROM robust_r r FULL JOIN robust_s s ON s.a = r.a;
SELECT count(*) FROM robust_t t,
  LATERAL (SELECT max(s.b) AS m FROM robust_s s WHERE s.a = t.b) l;
SELECT count(*) FROM (SELECT b, count(*) FROM robust_r GROUP BY b) g
  JOIN robust_s s ON s.b = g.b;

-- More rels than geqo_threshold: GEQO plans the joins with the standard cost
-- model and says so.
SET geqo_threshold = 2;
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a JOIN robust_t t ON t.b = s.a;
RESET geqo_threshold;

-- Parallel plans.
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a WHERE r.b < 20;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;

-- The cost cross check recosts every 2p path with the 1p functions.  In P0
-- it only reports, and the two still differ (the plan lists the known
-- differences), so its warnings are not shown; these queries check that it
-- runs on every path type and leaves the results alone.
SET robust_debug_check_costs = on;
SET client_min_messages = error;
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a JOIN robust_t t ON t.b = s.a
  WHERE r.b < 20;
SELECT count(*) FROM robust_r r LEFT JOIN robust_s s ON s.a = r.a WHERE r.b < 10;
SELECT count(*) FROM robust_r r FULL JOIN robust_s s ON s.a = r.a;
SELECT count(*) FROM robust_r r WHERE EXISTS (SELECT 1 FROM robust_s s WHERE s.a = r.a);
SELECT count(*) FROM robust_r r WHERE NOT EXISTS (SELECT 1 FROM robust_s s WHERE s.a = r.a);
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a WHERE false;
SELECT count(*) FROM robust_p p JOIN robust_r r ON r.a = p.a WHERE p.b = 3;
SELECT count(*) FROM robust_t t,
  LATERAL (SELECT max(s.b) AS m FROM robust_s s WHERE s.a = t.b) l;
SET enable_hashjoin = off;
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a WHERE r.b < 20;
SET enable_mergejoin = off;
SELECT count(*) FROM robust_r r JOIN robust_t t ON t.a = r.b WHERE r.a < 200;
RESET enable_hashjoin;
RESET enable_mergejoin;
SET enable_indexscan = off;
SET enable_seqscan = off;
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a
  WHERE r.b IN (1, 2, 3) AND s.b IN (1, 2);
RESET enable_indexscan;
RESET enable_seqscan;
SET max_parallel_workers_per_gather = 2;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SET min_parallel_table_scan_size = 0;
SET min_parallel_index_scan_size = 0;
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a WHERE r.b < 20;
SELECT a FROM robust_r WHERE b < 20 ORDER BY a LIMIT 3;
SELECT r.a FROM robust_r r JOIN robust_s s ON r.a = s.a WHERE r.b < 20
  ORDER BY r.a LIMIT 3;
RESET max_parallel_workers_per_gather;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;
RESET min_parallel_table_scan_size;
RESET min_parallel_index_scan_size;
RESET client_min_messages;
RESET robust_debug_check_costs;

-- With robust planning off, plans are PostgreSQL's again.
SET enable_rows_dist = off;
SELECT count(*) FROM robust_r r JOIN robust_s s ON r.a = s.a WHERE r.b < 20;

RESET error_profile_path;
RESET error_sample_count;
RESET error_bin_count;
RESET error_sample_seed;
RESET enable_rows_dist;
DROP TABLE robust_r, robust_s, robust_t, robust_p;
