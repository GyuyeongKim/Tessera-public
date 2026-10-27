CORE = ../core
B = build

$(B)/capi_test: capi_test.c $(B)/core/libtessera.a
	gcc -O2 -std=c11 -Wall -I$(CORE) -o $(B)/capi_test.o -c capi_test.c
	g++ -fopenmp -o $@ $(B)/capi_test.o $(B)/core/libtessera.a -ldl

$(B)/capi_int_test: capi_int_test.c $(B)/core/libtessera.a
	gcc -O2 -std=c11 -Wall -I$(CORE) -o $(B)/capi_int_test.o -c capi_int_test.c
	g++ -fopenmp -o $@ $(B)/capi_int_test.o $(B)/core/libtessera.a -ldl

$(B)/robust_test: robust_test.cpp $(B)/core/libtessera.a
	g++ -O2 -fopenmp -std=c++17 -I$(CORE) -o $@ robust_test.cpp $(B)/core/libtessera.a -ldl

$(B)/core/libtessera.a: FORCE
	$(MAKE) -C $(CORE) O=$(CURDIR)/$(B)/core

test: $(B)/capi_test $(B)/capi_int_test $(B)/robust_test
	./$(B)/capi_test
	./$(B)/capi_int_test
	./$(B)/robust_test --spill-dir $(B)

clean:
	rm -rf $(B)

FORCE:
.PHONY: test clean FORCE
