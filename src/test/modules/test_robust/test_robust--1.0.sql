/* src/test/modules/test_robust/test_robust--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_robust" to load this file. \quit

CREATE FUNCTION test_robustvec()
RETURNS text
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;

CREATE FUNCTION test_robust_dump()
RETURNS void
AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
