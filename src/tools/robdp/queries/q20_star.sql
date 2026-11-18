SELECT count(*) FROM t1, t2, t3, t4, t5 WHERE t2.fk = t1.id AND t3.fk = t1.id AND t4.fk = t1.id AND t5.fk = t1.id AND t1.b < 50 AND t2.a = 1
