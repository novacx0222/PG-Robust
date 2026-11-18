SELECT count(*) FROM t1, t2, t3, t4, t5, t6 WHERE t1.a = t2.a AND t2.a = t3.a AND t3.a = t4.a AND t4.a = t5.a AND t5.a = t6.a AND t1.b < 3
