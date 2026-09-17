# The C++ you need for these labs

The labs are about real-time behaviour, not about C++. The code is deliberately
close to C: functions, structs, loops, arrays and `printf`. This page explains
the handful of C++ features that do appear, and why each one is there. If you
know C, this is all you need.

## What the code avoids on purpose

No templates you have to write, no class hierarchies, no iterators, no
`auto` except where a cast makes the type obvious, no lambdas and no
algorithms from `<algorithm>` hidden behind a predicate. Loops are written
out so you can see what they cost — which matters when the point of the
exercise is how long something takes.

## What does appear

**`namespace { ... }`** around helper functions near the top of a file. It
means "private to this file", like `static` in C.

**`namespace rt`** is the shared header `common/rt.hpp`. `rt::now()` means the
function `now` from that namespace. Same idea as a `rt_` prefix in C.

**References: `int& x`, `const timespec& t`.** A reference is another name for
an existing variable. A parameter declared `const timespec& t` behaves like
`const timespec* t` in C, but you write `t.tv_sec` instead of `t->tv_sec`, and
it can never be null.

**`const` and `constexpr`.** `const int priority = ...` is a value that never
changes after it is set. `constexpr long kNsPerSec = 1'000'000'000L` is a
compile-time constant, the modern replacement for `#define`. The `'` marks are
digit separators: `1'000'000` is one million, and the quotes are not part of
the number.

**Struct initialisation: `timespec t{};`** The braces zero every field. In C
you would write `= {0}`. `sched_param sp{}; sp.sched_priority = 50;` is the
same pattern.

**`std::vector<long long> v;`** A growable array. `v.push_back(x)` appends,
`v[i]` indexes it, `v.size()` is the count. It frees itself. In real-time code
we call `v.reserve(n)` *before* the timing loop so the memory is allocated
once, up front — allocation inside the loop would add unpredictable delay.
Where a fixed size is known, the labs just use a plain array.

**Range-based for: `for (const Worker& w : workers) { ... }`** Visits every
element in turn. Equivalent to `for (i = 0; i < n; i++)` with `w` standing for
`workers[i]`.

**`std::printf` and `std::atoi`.** The same functions as in C. The `std::`
prefix comes from including `<cstdio>` rather than `<stdio.h>`.

**`std::atomic<bool>` and `std::atomic<int>`.** A variable several threads may
touch at the same time, where reads and writes cannot be torn or reordered
away. `flag.store(true)` writes, `flag.load()` reads. `volatile` is *not* a
substitute: it stops some compiler optimisations but promises nothing between
threads. In lab 7, `std::atomic` and its `compare_exchange_weak` are the
subject of the lab, and the README explains them there.

**`rt::Mutex` with `std::lock_guard`.**

```cpp
rt::Mutex m(rt::Mutex::kInherit);   // a mutex with priority inheritance
{
    std::lock_guard<rt::Mutex> guard(m);   // locks here
    shared_value++;
}                                          // unlocks here, whatever happens
```

`lock_guard` unlocks when it goes out of scope, so a lock cannot be left held
by an early `return`. `rt::Mutex` wraps a POSIX mutex because `std::mutex` has
no priority inheritance, which is exactly what lab 5 is about.

**`static_cast<int>(x)`** is a C cast, `(int)x`, spelled so the compiler can
check it.

**`nullptr`** is `NULL`.

## Why pthreads and not `std::thread`

`std::thread` cannot set a scheduling policy or a CPU before the thread
starts, so a real-time thread created with it begins life as an ordinary task.
The labs therefore call `pthread_create` through `rt::start_thread()`, which
sets policy, priority and affinity in the thread attributes first. Lab 1 shows
both side by side.

## Two traps worth remembering

1. **pthread functions return the error number.** They do *not* set `errno`,
   so `perror("pthread_create")` prints a wrong message. Use
   `strerror(rc)`. Same for `clock_nanosleep`.
2. **Check the return value of anything that asks for real-time behaviour.**
   If `pthread_setschedparam` fails with `EPERM` and you continue anyway, your
   program still runs and still prints numbers — they just say nothing about
   real-time scheduling. Every lab program stops instead.

## Compiling by hand

The Makefiles do this for you, but if you want to build one file yourself:

```
g++ -std=c++17 -O2 -Wall -Wextra -pthread -I../common -o latency latency.cpp
```

`-pthread` is required for threads, `-I../common` finds `rt.hpp`, and
`-Wall -Wextra` turn on the warnings you want to read.
