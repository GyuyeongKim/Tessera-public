MODULE_big = tessera_pg
OBJS = tessera_pg.o
EXTENSION = tessera_pg
DATA = tessera_pg--1.0.sql
REGRESS = tessera
PG_CONFIG ?= pg_config
T = ../core
TB = $(CURDIR)/build/core
PG_CPPFLAGS = -I$(T)
PG_CFLAGS = -Wno-declaration-after-statement
SHLIB_PREREQS = core
SHLIB_LINK += $(TB)/libtessera.a -lstdc++ -lgomp -ldl

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

core:
	$(MAKE) -C $(T) O=$(TB)
.PHONY: core
