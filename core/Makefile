O ?= build
CXX ?= g++
OPT = -O3 -fopenmp -fPIC -std=c++17 -Wall -Wno-unused-function $(EXTRA)
AMX_ARCH ?= -march=sapphirerapids
BASE_ARCH = -march=x86-64-v3
CORE = tessera.cpp tessera.hpp
OBJS = $(O)/core_amx.o $(O)/core_vnni.o $(O)/core_avx512.o $(O)/core_avx2.o $(O)/dispatch.o $(O)/common.o $(O)/query.o \
  $(O)/tessera_c.o

$(O)/libtessera.a: $(OBJS)
	rm -f $@
	ar rcs $@ $(OBJS)
$(O)/core_amx.o: tessera_amx.cpp $(CORE) | $(O)
	$(CXX) $(OPT) $(AMX_ARCH) -c -o $@ $<
$(O)/core_vnni.o: tessera_vnni.cpp $(CORE) | $(O)
	$(CXX) $(OPT) -march=x86-64-v4 -mavx512vnni -mavx512vbmi -mgfni -mbmi2 -c -o $@ $<
$(O)/core_avx512.o: tessera_avx512.cpp $(CORE) | $(O)
	$(CXX) $(OPT) -march=x86-64-v4 -c -o $@ $<
$(O)/core_avx2.o: tessera_avx2.cpp $(CORE) | $(O)
	$(CXX) $(OPT) $(BASE_ARCH) -c -o $@ $<
$(O)/dispatch.o: dispatch.cpp tessera.hpp | $(O)
	$(CXX) $(OPT) $(BASE_ARCH) -c -o $@ $<
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
.PHONY: clean
