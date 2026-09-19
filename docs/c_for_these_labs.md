# The C you need for these labs

The labs are about real-time behaviour, not about programming tricks. The code
is plain C11: functions, structs, arrays, loops and `printf`. Every real-time
call the labs teach (`pthread_*`, `clock_nanosleep`, `mlockall`, `sched_*`) is
a C function, so what you read in the labs is what you find in the man pages.

This page covers the few things that come up again and again.

## What the code avoids on purpose

No clever macros, no function pointers beyond the one `pthread_create` needs,
no `goto`, no bit tricks, no nested `?:` expressions. Loops are written out so
you can see what they cost, which matters when the question is how long
something takes.

## Things you will see

**`#include "rt.h"` as the first line.** `common/rt.h` holds the shared
real-time helpers, all named `rt_something`. It must be the first include
because it defines `_GNU_SOURCE`, a switch that has to be set before any
system header is read and that enables the Linux-specific calls (CPU pinning,
`sched_getcpu`). Labs 0 and 1 do not use `rt.h`; they put
`#define _GNU_SOURCE` at the top themselves.

**`static` in front of a function** means "private to this file". Every
helper in a lab program is `static`. In `rt.h` the functions are
`static inline`, which lets a header contain complete functions: including it
is enough, and there is nothing extra to link.

**`struct timespec`** is how POSIX represents time: `tv_sec` whole seconds
plus `tv_nsec` nanoseconds. The helpers pass it around by value, like a
number:

```c
struct timespec next = rt_now();
next = rt_add_ns(next, period_ns);      /* next + one period */
long long late = rt_diff_ns(rt_now(), next);
```

**Nanoseconds are `long long`.** One second is 1 000 000 000 ns, which no
longer fits comfortably in a 32-bit type. Print with `%lld`.
`RT_NS_PER_MS`, `RT_NS_PER_US` and `RT_NS_PER_SEC` convert units.

**Pointers to structs.** A thread function receives one `void *`. The labs
always pass the address of a struct and turn it back at the top of the
function:

```c
static void *worker(void *arg) {
    struct work *w = arg;      /* arg is the &work[i] given to pthread_create */
    w->sum = 42;               /* w->sum means (*w).sum */
    return NULL;
}
```

**Arrays with a fixed size, allocated before the timing loop.** `malloc`
inside a real-time loop can take an unpredictable time, and so can growing
anything. The labs size their storage up front:

```c
long long *samples = malloc(iterations * sizeof(samples[0]));   /* once, before */
for (long i = 0; i < iterations; i++) samples[i] = ...;          /* loop only stores */
```

**`volatile`** tells the compiler "do not optimise accesses to this variable
away". The busy loops use it so that `-O2` cannot delete them. It does
**not** make a variable safe to share between threads.

**Sharing a variable between threads** takes one of two tools:

- a mutex around every access:
  ```c
  pthread_mutex_lock(&lock);
  shared_value++;                 /* the critical section: keep it short */
  pthread_mutex_unlock(&lock);
  ```
  `rt_mutex_init(&lock, 1)` creates the mutex with priority inheritance,
  `rt_mutex_init(&lock, 0)` without. Lab 5 shows why that matters.
- an atomic variable from `<stdatomic.h>`, for a single flag or counter:
  ```c
  atomic_bool done = false;
  atomic_store(&done, true);      /* writer */
  if (atomic_load(&done)) ...     /* reader */
  ```
  Lab 7 is about what else atomics can do (`atomic_compare_exchange_weak`).

One exception: a flag set from a signal handler (Ctrl+C) is
`volatile sig_atomic_t`, the type the C standard guarantees for that job.

**`bool`, `true`, `false`** come from `<stdbool.h>`.

**`const`** on a variable or parameter means it is not modified after it is
set. It costs nothing and tells the reader what can change.

## Two traps worth remembering

1. **pthread functions return the error number.** They do *not* set `errno`,
   so `perror("pthread_create")` prints a wrong message. Write
   ```c
   int rc = pthread_create(...);
   if (rc != 0) fprintf(stderr, "pthread_create: %s\n", strerror(rc));
   ```
   The same goes for `clock_nanosleep`. Ordinary system calls such as
   `mlockall` and `fopen` do set `errno`, and `perror` is right for them.
2. **Check the return value of anything that asks for real-time behaviour.**
   If `pthread_setschedparam` fails with `EPERM` and you carry on anyway, the
   program still runs and still prints numbers. They just say nothing about
   real-time scheduling. Every lab program stops instead.

## Compiling by hand

The Makefiles do this for you. To build one file yourself:

```
gcc -std=c11 -O2 -Wall -Wextra -pthread -I../common -o latency latency.c
```

`-pthread` is required for threads, `-I../common` finds `rt.h`, and
`-Wall -Wextra` turn on the warnings you want to read. Lab 7 also needs
`-latomic` at the end, and programs that use `<math.h>` need `-lm`.
