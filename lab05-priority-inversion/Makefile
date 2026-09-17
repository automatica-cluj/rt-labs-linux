CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CXXFLAGS += $(if $(WERROR),-Werror)
CPPFLAGS += -I../common

all: inversion

inversion: inversion.cpp ../common/rt.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $<

# Quick functional check used by 'make smoke' at the top level: a plain mutex
# must show unbounded inversion, priority inheritance must bound it.
smoke: all
	./inversion none | tee /dev/stderr | grep -q '^UNBOUNDED'
	./inversion inherit | tee /dev/stderr | grep -q '^BOUNDED'

clean:
	rm -f inversion

.PHONY: all smoke clean
