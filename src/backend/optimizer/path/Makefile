#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for optimizer/path
#
# IDENTIFICATION
#    src/backend/optimizer/path/Makefile
#
#-------------------------------------------------------------------------

subdir = src/backend/optimizer/path
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	addpath.o \
	allpaths.o \
	clausesel.o \
	cost_check.o \
	cost_ext.o \
	costsize.o \
	equivclass.o \
	indxpath.o \
	joincost_ext.o \
	joinpath.o \
	joinrels.o \
	nodecost_ext.o \
	pathkeys.o \
	pathstrategy.o \
	pathstrategy_ext.o \
	tidpath.o

include $(top_srcdir)/src/backend/common.mk

# RobDP: the files imported from v20 declare variables after statements
# throughout; silence that one warning for them until they are reformatted.
addpath.o cost_ext.o joincost_ext.o pathstrategy.o pathstrategy_ext.o: CFLAGS += -Wno-declaration-after-statement
