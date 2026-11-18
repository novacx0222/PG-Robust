SELECT count(*) FROM t1, t2, t3, t4, t5, t6 WHERE t1.fk = t2.id AND t2.fk = t3.id AND t3.fk = t4.id AND t4.fk = t5.id AND t5.fk = t6.id AND t1.a < 10 AND t3.b = 5
