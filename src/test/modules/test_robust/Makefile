# src/test/modules/test_robust/Makefile

MODULE_big = test_robust
OBJS = \
	$(WIN32RES) \
	test_robust.o
PGFILEDESC = "test_robust - unit tests for RobDP robust planning"

EXTENSION = test_robust
DATA = test_robust--1.0.sql

REGRESS = test_robust

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_robust
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
