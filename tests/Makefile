CORE = ../core
B = build

$(B)/capi_test: capi_test.c $(B)/core/libtessera.a
	gcc -O2 -std=c11 -Wall -I$(CORE) -o $(B)/capi_test.o -c capi_test.c
	g++ -fopenmp -o $@ $(B)/capi_test.o $(B)/core/libtessera.a

$(B)/core/libtessera.a: FORCE
	$(MAKE) -C $(CORE) O=$(CURDIR)/$(B)/core

test: $(B)/capi_test
	./$(B)/capi_test

clean:
	rm -rf $(B)

FORCE:
.PHONY: test clean FORCE
