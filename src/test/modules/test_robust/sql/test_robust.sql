--
-- Unit tests for the RobDP support code added in step S1: the per-sample
-- vector type, the debug dump format and the GUCs.
--
CREATE EXTENSION test_robust;

-- RobustVec API (raises an error naming the failed check)
SELECT test_robustvec();

-- Exact dump format: %.17g numbers, "nan"/"inf" strings, JSON escaping
SET client_min_messages = log;
SELECT test_robust_dump();
RESET client_min_messages;

-- Defaults: robust planning is off
SELECT name, setting, context FROM pg_settings
WHERE name IN ('enable_rows_dist', 'error_sample_count', 'error_sample_seed',
               'error_bin_count', 'error_sample_kde_bandwidth',
               'error_profile_path', 'robust_debug_dump')
ORDER BY name;

-- Ranges
SET error_sample_count = 0;
SET error_sample_count = 1001;
SET error_sample_count = 1;
SET error_sample_count = 1000;
SET error_bin_count = 9;
SET error_sample_kde_bandwidth = -1.5;
RESET ALL;

-- The profile directory and the dump are superuser-only: an ordinary user
-- must not make the server read files or flood the log
CREATE ROLE regress_robust_user;
SET ROLE regress_robust_user;
SET error_profile_path = '/tmp';
SET robust_debug_dump = on;
SET enable_rows_dist = on;
SET error_sample_count = 5;
RESET ROLE;
RESET ALL;
DROP ROLE regress_robust_user;

-- The "query" event of standard_planner
SET robust_debug_dump = on;
SET error_sample_count = 7;
SET client_min_messages = log;
SELECT 1 AS one;
RESET client_min_messages;
RESET ALL;

-- Error profiles (S2).  Files are written to the data directory, and
-- error_profile_path = '.' (the server's working directory is the data
-- directory), so that messages show stable paths.
CREATE TEMP TABLE prof (line text);
INSERT INTO prof VALUES
  ('# comment and blank lines are ignored'), (''),
  ('0.5 0.1'), ('0.01 0.1'), ('0.2 0.2'), ('0 0.3'), ('1 0.9'), ('0.9 0.9'),
  ('not a number'), ('0.5'), ('0.5 0.5 0.5'), ('1.5 0.5'), ('0.5 -0.1'), ('nan 0.5');
DO $$ BEGIN
  EXECUTE format('COPY prof TO %L', current_setting('data_directory') || '/rt_a.txt');
  EXECUTE format('COPY (SELECT ''x y'') TO %L', current_setting('data_directory') || '/rt_empty.txt');
END $$;
SET error_profile_path = '.';
SET error_bin_count = 2;
SET robust_debug_dump = on;
SET client_min_messages = log;
-- 6 valid points, 6 invalid lines (5 reported one by one, then a total);
-- bins: sel_est {0.1, 0.1, 0.2} and {0.3, 0.9, 0.9}
SELECT test_robust_profile('rt_a', '{0, 0.1, 0.15, 0.2, 0.25, 0.9, 1}');
-- cached: no second load, no warnings
SELECT test_robust_profile('rt_a', '{0.2}');
-- a new bin count rebuilds the bins; the two points with sel_est 0.9 are
-- not split across bins, so the bin sizes are 2, 1, 3, 0
SET error_bin_count = 4;
SELECT test_robust_profile('rt_a', '{0.1, 0.2, 0.3, 0.9}');
RESET client_min_messages;
RESET robust_debug_dump;
-- no profile, no valid point, unsafe keys
SELECT test_robust_profile('rt_missing', '{0.5}');
SELECT test_robust_profile('rt_empty', '{0.5}');
SELECT test_robust_profile('../rt_a', '{0.5}');
SELECT test_robust_profile('.rt_a', '{0.5}');
-- no directory configured
RESET error_profile_path;
SELECT test_robust_profile('rt_a', '{0.5}');
RESET ALL;

-- Drawing errors (S3).  rt_a with 2 bins: bin 0 has the log ratios
-- log(5), log(0.1), 0 (estimates 0.1, 0.1, 0.2); bin 1 has log(1e-12/0.3),
-- log(1/0.9), 0.
SET error_profile_path = '.';
SET error_bin_count = 2;
SET error_sample_count = 10;
-- the points drawn (rounded, so that the output does not depend on the
-- last bit of log()); pins the seeding and the random stream
SELECT round(v::numeric, 6) AS x FROM unnest(test_robust_draw('rt_a', 'rt_a', 0.1)) v;
-- an estimate in bin 1
SELECT round(v::numeric, 6) AS x FROM unnest(test_robust_draw('rt_a', 'rt_a', 0.5)) v;
-- reproducible; the draws for N are a prefix of those for a larger N
SELECT test_robust_draw('rt_a', 'x', 0.1) = test_robust_draw('rt_a', 'x', 0.1) AS same;
SET error_sample_count = 1000;
CREATE TEMP TABLE d AS
  SELECT test_robust_draw('rt_a', 'x', 0.1) AS x,
         test_robust_draw('rt_a', 'y', 0.1) AS y,
         test_robust_draw('rt_a', 'x', 0.15) AS x2;
SET error_sample_count = 10;
SELECT x[1:10] = test_robust_draw('rt_a', 'x', 0.1) AS prefix FROM d;
SET error_sample_count = 1000;
-- bandwidth 0: every draw is a point of the bin, each point is drawn
SELECT array(SELECT DISTINCT round(v::numeric, 6) FROM unnest(x) v ORDER BY 1) AS points FROM d;
-- another instance of the same profile gets other draws; another estimate
-- in the same bin gets the same draws
SELECT x <> y AS independent, x = x2 AS same_bin FROM d;
-- a new seed gives other draws
SET error_sample_seed = 7;
SELECT test_robust_draw('rt_a', 'x', 0.1) <> x AS reseeded FROM d;
RESET error_sample_seed;
-- a fixed bandwidth adds noise: no draw is a point of the bin, and the
-- draws use the same points as without noise when the noise is removed
SET error_sample_kde_bandwidth = 1e-9;
SELECT bool_and(abs(v - p) < 1e-7 AND v <> p) AS jittered
FROM d, unnest(test_robust_draw('rt_a', 'x', 0.1), x) AS u(v, p);
-- Silverman (bandwidth < 0): bin 0's h is positive
SET error_sample_kde_bandwidth = -1;
SELECT count(DISTINCT v) > 3 AS smoothed FROM unnest(test_robust_draw('rt_a', 'x', 0.1)) v;
-- no profile
SELECT test_robust_draw('rt_missing', 'x', 0.1);
RESET ALL;
