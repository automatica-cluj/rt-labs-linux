# Lab 7 — Lock-free programming

In this lab you build and break a lock-free stack. You measure what it costs
compared with a mutex. You watch the ABA problem happen step by step and then
fix it. Finally you compare a mutex, a priority-inheritance mutex and
`std::atomic` in the one situation real-time programs care about: a
high-priority task sharing data with a low-priority one.

Time: about 2 hours. No sudo needed anywhere in this lab.

---

## 1. Background

### Blocking, lock-free, wait-free

These words describe *progress guarantees*, what a thread can count on no
matter what the other threads do:

| | guarantee | example |
|---|---|---|
| blocking | none: a thread holding a lock can stop everybody else | mutex |
| lock-free | some thread always makes progress; one thread may retry forever | Treiber stack, `fetch_add` loop |
| wait-free | every thread finishes in a bounded number of its own steps | `std::atomic::fetch_add` itself, per-thread ring buffer |

Lock-free does not mean "fast" and it does not mean "bounded". A
low-priority thread cannot block a high-priority thread, which is the part
that matters for real-time. But a thread whose compare-and-swap keeps losing
has no upper limit on its retries.

### Compare-and-swap

All lock-free code in this lab rests on one atomic instruction:

```cpp
// Atomically: if (head == expected) { head = desired; return true; }
//             else { expected = head; return false; }
head.compare_exchange_weak(expected, desired);
```

The `_weak` form may fail spuriously. That is fine inside a retry loop and
can be slightly cheaper. `_strong` fails only when the values differ.

### What `std::memory_order` is (and is not)

`load(std::memory_order_relaxed)` and friends take a hint about how *consistent*
concurrent reads need to be. It is not a CPU memory model and it is not a
"barrier". With GCC on x86-64 and arm64, `load`, `store` and `fetch_add`
produce the same instructions whatever order you pass. The default,
`memory_order_seq_cst`, is always correct. Use the defaults unless a profiler
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

The Makefile links `-latomic`. A 16-byte `std::atomic` (pointer plus tag)
compiles to a call into libatomic, which takes a small lock unless the CPU
has a double-width compare-and-swap. On x86-64 you can add `-mcx16` to allow
the `CMPXCHG16B` instruction. `aba_demo` prints what it got on your machine.

## 3. The lock-free stack

```
./lockfree_stack            # 4 threads, 200000 push+pop pairs each
./lockfree_stack 1          # one thread: no contention
```

Read `lockfree_stack.cpp` first. `push` and `pop` are about ten lines each.
Every thread pushes and pops at the same time. Afterwards the program checks
that every value was seen exactly once, then repeats the workload on a stack
protected by a mutex.

Output from the local Docker image (replace with lab-machine output):

```
Treiber stack: 4 threads, 200000 push+pop pairs each
std::atomic<Node*> is lock-free on this machine: yes

lock-free  : correct, 0.099 s, 16.17 M ops/s, 631642 CAS retries in pop
mutex      : correct, 0.049 s, 32.59 M ops/s

faster here: mutex (2.0x)
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
std::atomic<(pointer, tag)> is lock-free here: no (uses a lock)

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

## 5. Mutex, PI mutex and std::atomic

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
  mutex          39.94 M increments/s  count correct
  PI mutex        0.07 M increments/s  count correct
  std::atomic    93.02 M increments/s  count correct
  highest throughput here: std::atomic

Test 2: mixed priorities on CPU 6, 2 s per method
  high: FIFO 50, 5 ms period, 50 us update
  medium: FIFO 30, 23 ms period, 8 ms computation (no counter)
  low: FIFO 10, 7 ms period, 3 ms update
  mutex        high response over 376 samples: min 109 us, avg 1710 us, max 9816 us
  PI mutex     high response over 401 samples: min 55 us, avg 710 us, max 3050 us
  std::atomic  high response over 401 samples: min 50 us, avg 890 us, max 1987 us

What the numbers say on this run:
  lowest worst-case response for high: std::atomic (1987 us)
  plain mutex worst case is 3.2x the PI mutex worst case: priority
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
- **With `std::atomic`, low does its 3 ms of work on a private value and
  publishes it with one `fetch_add`.** High never waits for low at all. Its
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

In `LockFreeStack::pop`, replace the loop with a load followed by a plain
`store`:

```cpp
Node* old_head = head_.load();
if (old_head == nullptr) return nullptr;
head_.store(old_head->next);
return old_head;
```

Run `./lockfree_stack 4` a few times. What does the correctness check report,
and why? Restore the original afterwards.

**C. Real use-after-free, caught by AddressSanitizer**

```
make asan
./build/aba_demo_asan delete
```

This time node B is really `delete`d, as it would be in a naive
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

Both should run without `WARNING: ThreadSanitizer`. Now make the `plain_`
counter in `sync_perf.cpp` be incremented *outside* the lock in `update()`,
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
   When would you choose it over `std::atomic` anyway? (Hint: think about
   updates that touch more than one value.)
5. What does `std::memory_order_relaxed` promise, and why did choosing it not
   make the programs faster?
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
