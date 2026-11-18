#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/util
#
# IDENTIFICATION
#    src/backend/optimizer/util/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/util
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	appendinfo.o \
	clauses.o \
	ep.o \
	inherit.o \
	joininfo.o \
	kde.o \
	orclauses.o \
	paramassign.o \
	pathnode.o \
	placeholder.o \
	plancat.o \
	predtest.o \
	relnode.o \
	restrictinfo.o \
	sample.o \
	tlist.o \
	var.o

include $(top_srcdir)/src/backend/common.mk

# RobDP: the files imported from v20 declare variables after statements
# throughout; silence that one warning for them until they are reformatted.
ep.o kde.o sample.o: CFLAGS += -Wno-declaration-after-statement
