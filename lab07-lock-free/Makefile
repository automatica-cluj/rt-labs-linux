CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CXXFLAGS += $(if $(WERROR),-Werror)
CPPFLAGS += -I../common
# std::atomic of a 16-byte struct (aba_demo's tagged pointer) may call into
# libatomic. On x86-64, -mcx16 lets GCC use the CMPXCHG16B instruction.
LDLIBS   += -latomic

PROGRAMS := lockfree_stack aba_demo sync_perf

all: $(PROGRAMS)

%: %.cpp ../common/rt.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $< $(LDLIBS)

# ThreadSanitizer and AddressSanitizer builds go to build/.
build/%_tsan: %.cpp ../common/rt.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -g -fsanitize=thread -o $@ $< $(LDLIBS)

build/%_asan: %.cpp ../common/rt.hpp
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -g -fsanitize=address -fno-omit-frame-pointer -o $@ $< $(LDLIBS)

tsan: build/lockfree_stack_tsan build/aba_demo_tsan build/sync_perf_tsan

asan: build/aba_demo_asan build/lockfree_stack_asan

# Quick functional check used by 'make smoke' at the top level.
smoke: all
	./lockfree_stack 4 50000
	./aba_demo | grep -q 'ABA happened'
	./aba_demo | grep -q 'tagged CAS rejected stale head'
	./sync_perf 4 1

clean:
	rm -rf $(PROGRAMS) build

.PHONY: all tsan asan smoke clean
