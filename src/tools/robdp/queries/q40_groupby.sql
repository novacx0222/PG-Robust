SELECT t2.a, count(*) FROM t1 JOIN t2 ON t1.fk = t2.id WHERE t1.b < 100 GROUP BY t2.a
