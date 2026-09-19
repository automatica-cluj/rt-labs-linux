CC     = gcc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra -pthread
CFLAGS   += $(if $(WERROR),-Werror)
CPPFLAGS += -I../common
# An _Atomic 16-byte struct (aba_demo's tagged head) may call into libatomic.
# On x86-64, -mcx16 lets GCC use the CMPXCHG16B instruction instead.
LDLIBS   += -latomic

PROGRAMS := lockfree_stack aba_demo sync_perf

all: $(PROGRAMS)

%: %.c ../common/rt.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDLIBS)

# ThreadSanitizer and AddressSanitizer builds go to build/.
build/%_tsan: %.c ../common/rt.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -g -fsanitize=thread -o $@ $< $(LDLIBS)

build/%_asan: %.c ../common/rt.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -g -fsanitize=address -fno-omit-frame-pointer -o $@ $< $(LDLIBS)

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
