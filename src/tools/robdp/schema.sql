-- RobDP v2 benchmark schema.
--
-- Fully deterministic data, and a statistics target large enough that ANALYZE
-- reads every row of every table (300 * 1000 = 300k sample rows > the largest
-- table), so every build sees identical statistics on every load.
--
-- t1..t8: t<i> has 20000*i rows.  id is the key, fk points into t<i+1>
-- (chain joins), a has 100 values, b has 1000, c is skewed (most rows small).
-- p is range-partitioned, for partitionwise / append paths.

SET default_statistics_target = 1000;

DO $$
BEGIN
  FOR i IN 1..8 LOOP
    EXECUTE format('DROP TABLE IF EXISTS t%s CASCADE', i);
    EXECUTE format('CREATE TABLE t%s (id int PRIMARY KEY, fk int, a int, b int, c int)', i);
    EXECUTE format($q$INSERT INTO t%s
                     SELECT g, (g * 7919) %% %s + 1, g %% 100, g %% 1000,
                            floor(1000 * power(((g * 7919) %% 10007) / 10007.0, 4))::int
                     FROM generate_series(1, %s) g$q$,
                   i, 20000 * (i + 1), 20000 * i);
    EXECUTE format('CREATE INDEX ON t%s (fk)', i);
    EXECUTE format('CREATE INDEX ON t%s (a)', i);
  END LOOP;
END $$;

DROP TABLE IF EXISTS p CASCADE;
CREATE TABLE p (id int, v int) PARTITION BY RANGE (id);
CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (1) TO (25001);
CREATE TABLE p2 PARTITION OF p FOR VALUES FROM (25001) TO (50001);
CREATE TABLE p3 PARTITION OF p FOR VALUES FROM (50001) TO (75001);
CREATE TABLE p4 PARTITION OF p FOR VALUES FROM (75001) TO (100001);
INSERT INTO p SELECT g, g % 37 FROM generate_series(1, 100000) g;
CREATE INDEX ON p (id);

VACUUM ANALYZE;
