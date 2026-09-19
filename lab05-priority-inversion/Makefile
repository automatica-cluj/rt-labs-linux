CC     = gcc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -pthread
CFLAGS   += $(if $(WERROR),-Werror)
CPPFLAGS += -I../common

all: inversion

inversion: inversion.c ../common/rt.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

# Quick functional check used by 'make smoke' at the top level: a plain mutex
# must show unbounded inversion, priority inheritance must bound it.
smoke: all
	./inversion none | tee /dev/stderr | grep -q '^UNBOUNDED'
	./inversion inherit | tee /dev/stderr | grep -q '^BOUNDED'

clean:
	rm -f inversion

.PHONY: all smoke clean
