O ?= build
CXX ?= g++
OPT = -O3 -fopenmp -fPIC -std=c++17 -Wall -Wno-unused-function $(EXTRA)
AMX_ARCH ?= -march=sapphirerapids
BASE_ARCH = -march=x86-64-v3
CORE = tessera.cpp tessera.hpp
ARCH ?= $(shell uname -m)
ifeq ($(filter $(ARCH),x86_64 amd64),)
BASE_ARCH =
OBJS = $(O)/common.o $(O)/query.o $(O)/tessera_c.o $(O)/core_portable.o $(O)/dispatch.o
else
OBJS = $(O)/common.o $(O)/query.o $(O)/tessera_c.o $(O)/core_avx2.o $(O)/dispatch.o $(O)/core_avx512.o \
  $(O)/core_vnni.o $(O)/core_amx.o
endif

SOVER = 3
VERSION = $(shell sed -n 's/.*kVersion = "\(.*\)";/\1/p' tessera.hpp)
PREFIX ?= /usr/local
LDFLAGS ?=
ifeq ($(shell uname -s),Darwin)
SHLIB = libtessera.$(SOVER).dylib
SHLINK = libtessera.dylib
SHFLAGS = -dynamiclib -install_name @rpath/$(SHLIB) -Wl,-exported_symbols_list,libtessera.exp
SHDEP = libtessera.exp
LIBS_PRIVATE = -lc++ -lomp
else
SHLIB = libtessera.so.$(SOVER)
SHLINK = libtessera.so
SHFLAGS = -shared -Wl,-soname,$(SHLIB) -Wl,--version-script=libtessera.map -Wl,-z,defs
SHDEP = libtessera.map
LIBS_PRIVATE = -lstdc++ -lgomp -ldl
endif

$(O)/libtessera.a: $(OBJS)
	rm -f $@
	$(CXX) -r -nostdlib -o $(O)/tessera_all.o $(OBJS)
	ar rcs $@ $(O)/tessera_all.o
$(O)/$(SHLIB): $(OBJS) $(SHDEP)
	$(CXX) $(SHFLAGS) -fopenmp -o $@ $(OBJS) $(LDFLAGS) $(filter -ldl,$(LIBS_PRIVATE))
	ln -sf $(SHLIB) $(O)/$(SHLINK)
shared: $(O)/$(SHLIB)
install: $(O)/libtessera.a $(O)/$(SHLIB)
	mkdir -p $(PREFIX)/lib/pkgconfig $(PREFIX)/include/tessera
	cp $(O)/libtessera.a $(O)/$(SHLIB) $(PREFIX)/lib/
	ln -sf $(SHLIB) $(PREFIX)/lib/$(SHLINK)
	cp tessera_c.h $(PREFIX)/include/tessera/
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@VERSION@|$(VERSION)|' -e 's|@LIBS_PRIVATE@|$(LIBS_PRIVATE)|' tessera.pc.in \
	  > $(PREFIX)/lib/pkgconfig/tessera.pc
$(O)/core_amx.o: tessera_amx.cpp $(CORE) | $(O)
	$(CXX) $(OPT) $(AMX_ARCH) -c -o $@ $<
$(O)/core_vnni.o: tessera_vnni.cpp $(CORE) | $(O)
	$(CXX) $(OPT) -march=x86-64-v4 -mavx512vnni -mavx512vbmi -mgfni -mbmi2 -c -o $@ $<
$(O)/core_avx512.o: tessera_avx512.cpp $(CORE) | $(O)
	$(CXX) $(OPT) -march=x86-64-v4 -c -o $@ $<
$(O)/core_avx2.o: tessera_avx2.cpp $(CORE) | $(O)
	$(CXX) $(OPT) $(BASE_ARCH) -c -o $@ $<
$(O)/core_portable.o: tessera_portable.cpp $(CORE) | $(O)
	$(CXX) $(OPT) -c -o $@ $<
$(O)/dispatch.o: dispatch.cpp tessera.hpp tessera_c.h | $(O)
	$(CXX) $(OPT) -c -o $@ $<
$(O)/common.o: common.cpp tessera.hpp | $(O)
	$(CXX) $(OPT) $(BASE_ARCH) -c -o $@ $<
$(O)/query.o: query.cpp query.hpp tessera.hpp | $(O)
	$(CXX) $(OPT) $(BASE_ARCH) -c -o $@ $<
$(O)/tessera_c.o: tessera_c.cpp tessera_c.h query.hpp tessera.hpp | $(O)
	$(CXX) $(OPT) $(BASE_ARCH) -c -o $@ $<
$(O):
	mkdir -p $@
clean:
	rm -rf $(O)
.PHONY: clean shared install
