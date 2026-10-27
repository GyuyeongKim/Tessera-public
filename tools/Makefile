CORE = ../core
B = build

$(B)/calibrate_run: calibrate_run.c $(B)/core/libtessera.a
	gcc -O2 -std=c11 -Wall -I$(CORE) -o $(B)/calibrate_run.o -c calibrate_run.c
	g++ -fopenmp -o $@ $(B)/calibrate_run.o $(B)/core/libtessera.a

$(B)/core/libtessera.a: FORCE
	$(MAKE) -C $(CORE) O=$(CURDIR)/$(B)/core

clean:
	rm -rf $(B)

FORCE:
.PHONY: clean FORCE
