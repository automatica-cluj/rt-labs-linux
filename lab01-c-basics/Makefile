CC     = gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pthread
CFLAGS += $(if $(WERROR),-Werror)
LDLIBS += -lm

PROGRAMS = time_basics threads file_io busy_wait scheduling statistics

all: $(PROGRAMS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Quick functional check used by 'make smoke' at the top level.
smoke: all
	./time_basics | grep -q 'late by us'
	./threads | grep -q 'sum of 0..2999999 = 4499998500000'
	./file_io /tmp/lab01_smoke.txt | grep -q 'wrote 10'
	test $$(grep -vc '^#' /tmp/lab01_smoke.txt) -eq 10
	./busy_wait 10 2 | grep -q 'average overrun'
	! ./busy_wait abc 2>/dev/null
	./scheduling 50 | grep -q 'SCHED_FIFO, priority 50'
	! ./scheduling 90 >/dev/null
	./statistics 7 | grep -q 'p99.9'

clean:
	rm -f $(PROGRAMS) measurements.txt

.PHONY: all smoke clean
