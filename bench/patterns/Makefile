# bench/patterns — allocation-counting interposer for run_patterns.py
CC ?= cc
CFLAGS ?= -O2 -Wall

all: liballoccount.dylib

liballoccount.dylib: alloccount.c
	$(CC) $(CFLAGS) -dynamiclib -o $@ $<

clean:
	rm -f liballoccount.dylib
	rm -rf build

.PHONY: all clean
