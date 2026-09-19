CC     = gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pthread
CFLAGS += $(if $(WERROR),-Werror)
CPPFLAGS += -I../common

all: latency

latency: latency.c ../common/rt.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

# Quick functional check used by 'make smoke' at the top level.
smoke: all
	./latency 0 1 abs 1000 /tmp/lab02_other.csv | grep -q 'SCHED_OTHER'
	./latency 50 1 abs 1000 /tmp/lab02_fifo.csv | grep -q 'SCHED_FIFO priority 50'
	./latency 50 1 rel | grep -q 'drift'
	! ./latency 90 1 2>/dev/null
	! ./latency 50 1 sideways 2>/dev/null
	python3 plot_latency.py /tmp/lab02_other.csv /tmp/lab02_fifo.csv | grep -q 'plot saved'

clean:
	rm -f latency *.csv *.png

.PHONY: all smoke clean
