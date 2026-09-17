CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CXXFLAGS += $(if $(WERROR),-Werror)
CPPFLAGS += -I../common

PROGRAMS = deadlock lock_order

all: $(PROGRAMS)

%: %.cpp ../common/rt.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

# Quick functional check used by 'make smoke' at the top level.
smoke: all
	./deadlock 300 > smoke_timeout.log && grep -q 'deadlock detected' smoke_timeout.log
	./deadlock 0 > smoke_deadlock.log; test $$? -eq 3 && grep -q 'DEADLOCK: both threads stuck' smoke_deadlock.log
	./lock_order ordered | grep -q 'completed, no deadlock'
	./lock_order trylock | grep -q 'completed, no deadlock'
	./lock_order scoped 50 | grep -q 'completed, no deadlock'
	./lock_order wrong > smoke_wrong.log; test $$? -eq 3
	rm -f smoke_*.log

clean:
	rm -f $(PROGRAMS) smoke_*.log

.PHONY: all smoke clean
