# Lab 7 — Lock-free programming

In this lab you build and break a lock-free stack. You measure what it costs
compared with a mutex. You watch the ABA problem happen step by step and then
fix it. Finally you compare a mutex, a priority-inheritance mutex and an
atomic variable in the one situation real-time programs care about: a
high-priority task sharing data with a low-priority one.

The programs are plain C11. The only new tool is `<stdatomic.h>`: a variable
declared `_Atomic(...)` (or `atomic_long`, `atomic_bool`) may be shared between
threads and is touched only through `atomic_load`, `atomic_store`,
`atomic_fetch_add` and `atomic_compare_exchange_weak/strong`.

Time: about 2 hours. No sudo needed anywhere in this lab.

Theory behind this lab (concept pages, in Romanian):
[Condiții de cursă](https://github.com/automatica-cluj/rt-concepts/blob/main/5-shared-resources/race-conditions.md),
[Garanții de progres](https://github.com/automatica-cluj/rt-concepts/blob/main/6-lock-free/progress-guarantees.md),
[Compară-și-schimbă și problema ABA](https://github.com/automatica-cluj/rt-concepts/blob/main/6-lock-free/compare-and-swap-aba.md).

---

## 1. Background

### Blocking, lock-free, wait-free

These words describe *progress guarantees*, what a thread can count on no
matter what the other threads do:

| | guarantee | example |
|---|---|---|
| blocking | none: a thread holding a lock can stop everybody else | mutex |
| lock-free | some thread always makes progress; one thread may retry forever | Treiber stack, any compare-and-swap retry loop |
| wait-free | every thread finishes in a bounded number of its own steps | a single `atomic_fetch_add`, per-thread ring buffer |

Lock-free does not mean "fast" and it does not mean "bounded". A
low-priority thread cannot block a high-priority thread, which is the part
that matters for real-time. But a thread whose compare-and-swap keeps losing
has no upper limit on its retries.

### Compare-and-swap

All lock-free code in this lab rests on one atomic instruction:

```c
/* In one indivisible step:
 *   if (head == expected) { head = desired;  return true;  }
 *   else                  { expected = head; return false; }   */
atomic_compare_exchange_weak(&head, &expected, desired);
```

A failed compare-and-swap (CAS) is not a thread being blocked. It fails
because another thread's CAS succeeded in between, so somebody made progress.
That is the definition of lock-free.

The `_weak` form may fail spuriously. That is fine inside a retry loop and
can be slightly cheaper. `_strong` fails only when the values differ.

### The `_explicit` variants and memory order

Every atomic function has a second form, such as
`atomic_load_explicit(&x, memory_order_relaxed)`, that takes a *memory order*.
It is a hint to the compiler about how much synchronisation the program
needs. It is not the CPU's memory ordering and it is not a "barrier". With GCC
on x86-64 and arm64, load, store and fetch-add compile to the same
instructions whatever order you pass. The plain forms used in this lab mean
`memory_order_seq_cst`, which is always correct. Use them unless a profiler
tells you otherwise.

### The ABA problem

```
thread 1 reads:   head -> A -> B -> C          (plans CAS head: A -> B)
thread 2:         pop A, pop B, free B, reuse A, push A
now:              head -> A -> C               (B is freed)
thread 1 CAS:     head == A ? yes -> head = B  (B is freed memory!)
result:           head -> B(freed) -> C
```

The CAS compares *addresses*. The head went A, then B, then A again, and the
CAS cannot tell. Fixes:

- **Never reuse a node while anyone may still hold its address.**
  `lockfree_stack` does this the simple way: nodes are freed only after all
  threads have finished.
- **Tagged pointer.** Store (pointer, counter) and bump the counter on every
  change. A stale CAS then sees a different counter. `aba_demo` does this.
- **Safe memory reclamation.** This is how real programs do it:
  - *Hazard pointers:* a thread publishes the pointer it is about to use, and
    nobody frees published pointers.
  - *Epoch-based reclamation:* a node is freed when every thread has passed
    a later epoch.
  - *RCU:* the kernel's variant of the same idea.

## 2. Build

```
cd rt-labs/lab07-lock-free
make
```

The Makefile links `-latomic`. A 16-byte `_Atomic` struct (pointer plus tag)
compiles to a call into libatomic, which takes a small lock unless the CPU
has a double-width compare-and-swap. On x86-64 you can add `-mcx16` to allow
the `CMPXCHG16B` instruction. `aba_demo` prints what it got on your machine.

## 3. The lock-free stack

```
./lockfree_stack            # 4 threads, 200000 push+pop pairs each
./lockfree_stack 1          # one thread: no contention
```

Read `lockfree_stack.c` first. `lf_push` and `lf_pop` are about ten lines each,
and the three steps (read the head, prepare, compare-and-swap) are marked.
All nodes come from one array allocated before the threads start: no `malloc`
in the time-critical path, and no node is freed while a thread may still read it.
Every thread pushes and pops at the same time. Afterwards the program checks
that every value was seen exactly once, then repeats the workload on a stack
protected by a mutex.

Output from the local Docker image (replace with lab-machine output):

```
Treiber stack: 4 threads, 200000 push+pop pairs each
_Atomic(struct node *) is lock-free on this machine: yes

lock-free  : correct, 0.112 s, 14.31 M ops/s, 838732 CAS retries in pop
mutex      : correct, 0.037 s, 43.60 M ops/s

faster here: mutex (3.0x)
Throughput is an average. Neither number says anything about the worst case.
```

Yes, the mutex won. Every failed CAS repeats work, and the shared head bounces
between CPU caches. A futex-based mutex that is held for a few nanoseconds is
very cheap. Lock-free is not a performance trick. Its value is in what it
guarantees.

## 4. ABA, step by step

```
./aba_demo
```

Two threads are forced through the exact interleaving from section 1, once
with a plain pointer CAS and once with a tagged pointer.

Output from the local Docker image (replace with lab-machine output):

```
_Atomic(struct tagged) is lock-free here: no (uses a lock)

=== plain pointer CAS ===
initial stack: A(1) -> B(2) -> C(3)
thread 1: read head A, next B, then gets preempted
thread 2: popped A(1) and B(2), frees B
thread 2: pushed recycled node A(4). Stack: A(4) -> C(3)
thread 1: CAS(head, A -> B) succeeded
thread 1 popped value 4
stack now: B(2) [FREED] -> C(3) -> end
ABA happened: the head was A again, so the stale CAS succeeded.
  The stack now starts at freed node B, value 2 is 'in' the stack a
  second time and the 2 left entries should have been 1.

=== tagged pointer CAS ===
initial stack: A(1) -> B(2) -> C(3)
thread 1: read head A tag 3, next B, then gets preempted
thread 2: popped A(1) and B(2), frees B
thread 2: pushed recycled node A(4). Stack: A(4) -> C(3)
thread 1: CAS failed, head is now A tag 6. Retrying.
thread 1 popped value 4
stack now: C(3) -> end
tagged CAS rejected stale head: the tag changed while thread 1 was away.
  The retry popped the real head, 1 entry left, no freed node reachable.
```

In this demo "free" only marks the node, so the program can print what went
wrong. In real code the memory goes back to `malloc`. See experiment C.

The tagged head is `struct tagged { struct node *ptr; unsigned long tag; }`.
A CAS on a struct compares all of its bytes, so the struct must have no
padding (two 8-byte fields) and is always zeroed before use.

## 5. Mutex, PI mutex and atomic

```
./sync_perf                 # 4 threads, 2 s per method, CPU from your uid
```

- **Test 1 (throughput):** four normal threads increment a shared counter for
  2 seconds, using each method in turn.
- **Test 2 (mixed priority):** three `SCHED_FIFO` threads pinned to *one* CPU:
  - high: priority 50, every 5 ms, a 50 µs update
  - medium: priority 30, every 23 ms, 8 ms of computation that does not
    touch the counter
  - low: priority 10, every 7 ms, a 3 ms update

  For every period of the high thread the program records how long it took
  from release to finished update. The summary line reports the maximum.

Output from the local Docker image (replace with lab-machine output):

```
Test 1: throughput, 4 SCHED_OTHER threads, 2 s per method
  mutex          40.12 M increments/s  count correct
  PI mutex        0.07 M increments/s  count correct
  atomic         82.62 M increments/s  count correct
  highest throughput here: atomic

Test 2: mixed priorities on CPU 6, 2 s per method
  high: FIFO 50, 5 ms period, 50 us update
  medium: FIFO 30, 23 ms period, 8 ms computation (no counter)
  low: FIFO 10, 7 ms period, 3 ms update
  mutex        high response over 370 samples: min 132 us, avg 1447 us, max 9388 us
  PI mutex     high response over 401 samples: min 107 us, avg 867 us, max 3014 us
  atomic       high response over 401 samples: min 50 us, avg 429 us, max 1516 us

What the numbers say on this run:
  lowest worst-case response for high: atomic (1516 us)
  plain mutex worst case is 3.1x the PI mutex worst case: priority
  inversion through the medium thread was observed.
```

The Docker image does not run a `PREEMPT_RT` kernel and its timers are coarse,
so every number above is inflated. On the lab machine the minimums and
averages are far smaller. Look at how the three methods compare to each other.

Things to notice:

- **The PI mutex collapses in test 1.** A contended priority-inheritance lock
  always goes through the kernel (`FUTEX_LOCK_PI`), which tracks the owner so
  it can boost it. You pay for bounded blocking with throughput.
- **With the plain mutex in test 2, high got fewer than 400 samples.** Some
  periods were so late that the thread skipped them (read the overrun policy
  in `periodic_worker`).
- **With the atomic counter, low does its 3 ms of work on its own and
  publishes it with one `atomic_fetch_add`.** High never waits for low at all. Its
  worst case is left with only its own wake-up latency, because no thread on
  that CPU has a higher priority.

## 6. Experiments

Keep `htop` open in a second tmux pane. Each experiment below runs for only a
few seconds.

**A. Contention and CAS retries**

```
./lockfree_stack 1
./lockfree_stack 2
./lockfree_stack 8
```

Plot or tabulate ops/s and retries against the number of threads. When does
the lock-free stack beat the mutex, if ever? Keep in mind that your account is
limited to two CPUs of time on the shared machine.

**B. Break the stack on purpose**

In `lf_pop`, replace the loop with a load followed by a plain store:

```c
struct node *old_head = atomic_load(&s->head);
if (old_head == NULL) return NULL;
atomic_store(&s->head, old_head->next);
return old_head;
```

Run `./lockfree_stack 4` a few times. What does the correctness check report,
and why? Restore the original afterwards.

**C. Real use-after-free, caught by AddressSanitizer**

```
make asan
./build/aba_demo_asan delete
```

This time node B is really passed to `free()`, as it would be in a naive
implementation. ASan stops at the first read of freed memory. Read the three
stack traces: where it was read, who freed it, who allocated it. Without ASan
this program would carry on silently with corrupted data, which is why the
normal build refuses to run `delete` mode.

**D. Data races, caught by ThreadSanitizer**

```
make tsan
./build/lockfree_stack_tsan 4 20000
./build/sync_perf_tsan 2 1
```

Both should run without `WARNING: ThreadSanitizer`. Now make the `plain`
counter in `sync_perf.c` be incremented *outside* the lock in
`counter_update()`,
rebuild with `make tsan`, and run again. Programs run 5 to 15 times slower
under TSan. Do not compare any timings from these builds.

**E. Stretch: priority inversion for real**

Rerun `./sync_perf 4 10` so each method gets 10 seconds. Then, in a second
pane, run `stress-ng --cpu 2 --timeout 60` and repeat. Which worst case moves
the most, and why do the three `SCHED_FIFO` threads barely notice a
`SCHED_OTHER` load? Then change the medium thread's computation from 8 ms to
20 ms and predict the plain-mutex worst case before you measure it.

## 7. Questions

Write your answers in a short text file. They will be discussed at the start
of the next lab.

1. Why is `lockfree_stack` free of ABA even though it uses a plain pointer
   CAS? What exactly would you have to change to make ABA possible?
2. Explain in your own words why the tagged CAS in `aba_demo` fails. Could
   the tag ever wrap around, and would that matter in practice?
3. Is the Treiber stack lock-free or wait-free? Describe an execution in
   which one thread never completes its `pop`.
4. The PI mutex had the worst throughput and one of the best worst cases.
   When would you choose it over an atomic anyway? (Hint: think about
   updates that touch more than one value.)
5. The programs use the plain atomic functions, not the `_explicit` ones with
   `memory_order_relaxed`. What would `relaxed` promise, and why would it not
   make these programs faster on this machine?
6. Hazard pointers and epoch-based reclamation both *delay* freeing memory.
   What does that delay cost a real-time system, and when?

## House rules

- The mixed-priority test runs three real-time threads on one CPU for a few
  seconds per method. Do not loop it in a script. `pkill sync_perf` stops it.
- Choose your own CPU with the last argument (`./sync_perf 4 2 3`). The default
  comes from your user id, so classmates end up on different CPUs.
- Load generators always get a `--timeout`.
- Everything on this machine is wiped at the end of the term. Keep copies of
  your work.
