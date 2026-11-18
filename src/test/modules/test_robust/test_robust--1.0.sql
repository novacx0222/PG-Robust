/* src/test/modules/test_robust/test_robust--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_robust" to load this file. \quit

CREATE FUNCTION test_robustvec()
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_robust_dump()
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Load (or reuse) the error profile of a querylet and return the bin of
-- each estimate; NULL if the querylet has no profile
CREATE FUNCTION test_robust_profile(key text, sel_est float8[])
RETURNS int[]
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

-- Draw error_sample_count errors for querylet instance "inst" from the
-- profile "key"; NULL if there is no such profile
CREATE FUNCTION test_robust_draw(key text, inst text, sel_est float8)
RETURNS float8[]
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
