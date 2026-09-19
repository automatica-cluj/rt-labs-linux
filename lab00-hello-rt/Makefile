CC     = gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -pthread
CFLAGS += $(if $(WERROR),-Werror)

all: hello_rt

hello_rt: hello_rt.c
	$(CC) $(CFLAGS) -o $@ $<

# Quick functional check used by 'make smoke' at the top level.
smoke: all
	./hello_rt 0 1
	./hello_rt 50 1 | grep -q SCHED_FIFO
	./hello_rt 90 1 2>&1 | grep -q 'ulimit -r'   # must be refused with a hint

clean:
	rm -f hello_rt

.PHONY: all smoke clean
