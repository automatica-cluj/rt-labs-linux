CC     = gcc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -pthread
CFLAGS   += $(if $(WERROR),-Werror)
CPPFLAGS += -I../common
LDLIBS   += -lm

BINS = periodic_task control_app

all: $(BINS)

%: %.c ../common/rt.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

# Quick functional check used by 'make smoke' at the top level.
smoke: all
	./periodic_task 0 1 | grep -q RESULT
	./periodic_task 50 1 | grep -q SCHED_FIFO
	./periodic_task 50 1 1 1000 1500 | grep -q 'RESULT: deadline misses'   # overload must be detected
	./periodic_task 90 1 2>&1 | grep -q 'ulimit -r'                         # over the rtprio limit
	./control_app 0 1 | grep -q RESULT
	./control_app 70 1 | grep -q 'schedulable by the bound'
	./control_app 70 1 1 400 | grep -q 'RESULT: [0-9]* deadline misses'     # U = 204 %, must miss
	./control_app 90 1 2>&1 | grep -q 'ulimit -r'

clean:
	rm -f $(BINS)

.PHONY: all smoke clean
