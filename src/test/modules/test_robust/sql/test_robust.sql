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
