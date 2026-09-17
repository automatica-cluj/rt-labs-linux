CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CXXFLAGS += $(if $(WERROR),-Werror)
CPPFLAGS += -I../common

PROGS := fifo_order same_priority policy_mix

all: $(PROGS)

%: %.cpp timeline.hpp ../common/rt.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

# Quick functional check used by 'make smoke' at the top level.
smoke: all
	./fifo_order 0 | grep -q 'workers interleaved: yes'
	./fifo_order 70 | grep -q 'result: finished in priority order, one after another'
	sleep 1   # let the real-time budget of the CPU recover between runs
	./same_priority other | grep -q 'took turns'
	./same_priority fifo 50 | grep -q 'one after another'
	sleep 1
	./same_priority rr 50 | grep -q 'took turns'
	sleep 1
	./policy_mix 50 | grep -q 'finished last'
	./fifo_order 80 2>&1 | grep -q 'must be an integer'

clean:
	rm -f $(PROGS)

.PHONY: all smoke clean
