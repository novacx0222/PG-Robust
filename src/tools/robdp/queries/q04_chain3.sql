SELECT * FROM t1, t2, t3 WHERE t1.fk = t2.id AND t2.fk = t3.id AND t1.a < 10 AND t3.b = 5
