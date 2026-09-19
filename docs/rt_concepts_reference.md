# Real-time concepts: quick reference

Short definitions and formulas for all labs. Each topic gives the lab where it
is practised.

## Timing

| Term | Meaning |
|---|---|
| **Latency** | Actual start (or wake-up) time minus intended time. Never negative. Lab 2. |
| **Jitter** | Variation of latency across periods; often reported as max - min. Lab 2. |
| **Response time** | Release to completion of one job: latency + execution + preemption + blocking. Lab 4. |
| **WCET** | Worst-case execution time C of a task on given hardware. Measured values are only a lower bound. Lab 4. |
| **Deadline** | Latest acceptable completion time, D. Usually D = T (the period). |
| **Determinism** | The worst case is bounded and known. Average speed is secondary. |

**Hard real-time:** a missed deadline is a failure (airbag, motor current
loop). **Soft real-time:** a miss degrades quality (audio, video). **Firm:** a
late result is worthless but not harmful.

Report the maximum, and a high percentile (p99.9) next to it. An average hides
exactly the events that miss deadlines.

## Periodic tasks and schedulability (Lab 4)

A task τi has period Ti, worst-case execution time Ci and deadline Di (= Ti here).

- Utilisation: U = Σ Ci / Ti. U > 1 on one CPU can never be scheduled.
- **Rate monotonic (RM)**: fixed priorities, where a shorter period gets a
  higher priority. It is optimal among fixed-priority schedulers.
  - Liu & Layland sufficient test: U ≤ n(2^(1/n) - 1). The bound is 1.0 for
    n=1, 0.828 for n=2, 0.780 for n=3, and 0.693 as n → ∞.
  - Failing this test does not prove the tasks are unschedulable. Use response
    time analysis instead: Ri = Ci + Bi + Σ_{j∈hp(i)} ⌈Ri / Tj⌉ Cj, iterate,
    and the tasks are schedulable if Ri ≤ Di.
- **Earliest deadline first (EDF)**: dynamic priorities. Schedulable if and only
  if U ≤ 1 (with D = T). On Linux, `SCHED_DEADLINE` is EDF with reservations.
- Linux fixed priorities: `SCHED_FIFO` 1..99. On the lab machine students may
  use 1..80.

Periodic loop pattern, without drift:

```c
struct timespec next = rt_now();
for (;;) {
    next = rt_add_ns(next, period_ns);
    rt_sleep_until(next);         /* clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) */
    do_job();
    if (rt_diff_ns(rt_now(), next) > period_ns) { /* overrun: count it, skip ahead */ }
}
```

A relative sleep ("sleep one period after the job") adds every delay to all
later releases (Lab 2, experiment D).

## Preparing a real-time process (Labs 0, 2)

1. `mlockall(MCL_CURRENT | MCL_FUTURE)`: no page faults later.
   `mallopt(M_TRIM_THRESHOLD, -1)` and `mallopt(M_MMAP_MAX, 0)` keep the heap
   mapped.
2. Allocate and touch all memory (buffers, vectors, stacks) before the
   time-critical loop.
3. Create threads with explicit scheduling attributes (see
   `scheduling_policies_guide.md`) and check the return value.
4. Pin to a CPU when the experiment depends on threads competing.
5. No `printf`, file I/O, `malloc` or unbounded loops inside the loop. Record
   the data, and report it afterwards.

## Synchronisation

### Priority inversion (Lab 5)

High (H) waits for a lock held by Low (L). Medium (M) does not need the lock,
but preempts L, so H waits for M as well. With several medium tasks the wait
has no bound: **unbounded priority inversion** (Mars Pathfinder, 1997).

- **Priority inheritance (PIP):** while L holds a lock that H waits for, L runs
  at H's priority. H's blocking time is then bounded by the critical sections
  it has to wait for. `PTHREAD_PRIO_INHERIT`, or `rt_mutex_init(&m, 1)`.
- **Priority ceiling (PCP / PTHREAD_PRIO_PROTECT):** a lock carries the highest
  priority of any task that uses it. Whoever holds it runs at that priority.
  This also prevents deadlock between the locks involved.
- On `PREEMPT_RT`, the kernel's own spinlocks become rt_mutexes with priority
  inheritance.
- A mutex created with default attributes has no priority inheritance.

Blocking bound with PIP: Bi ≤ Σ over lower-priority tasks of their longest
critical section on a lock that τi (or a higher-priority task) can also lock.

### Deadlock (Lab 6)

The Coffman conditions must all hold at once:

| Condition | Break it by |
|---|---|
| Mutual exclusion | lock-free data structures, read-copy-update |
| Hold and wait | acquire all locks at once, or `pthread_mutex_trylock` and back off |
| No preemption | timed locks that give up and release (`pthread_mutex_clocklock`) |
| Circular wait | a global lock order: always lock m1 before m2 |

Detection by timeout tells you a lock took too long. It doesn't prove there
was a cycle, and recovering (release, back off, retry) costs time and adds
jitter. In real-time code prefer prevention: a fixed lock order, or a single
lock.

### Lock-free programming (Lab 7)

| Guarantee | Meaning |
|---|---|
| Blocking | a thread can be stopped indefinitely by another (locks) |
| Obstruction-free | a thread running alone finishes in bounded steps |
| Lock-free | some thread always makes progress; one thread may retry forever |
| Wait-free | every thread finishes in bounded steps (e.g. a single `fetch_add`) |

- **CAS** (`atomic_compare_exchange_weak` / `_strong`): "if the value is still what I read,
  replace it", as one atomic step. On failure you get the current value and
  retry.
- **ABA:** a thread reads A, gets preempted; others pop A, pop B and push A
  back (the same address, reused). The CAS succeeds, but the `next` pointer it
  saved is stale. Fixes: tagged pointers (pointer plus a counter that changes
  on every update), or safe memory reclamation so an address can't be reused
  while someone may still hold it (hazard pointers, epoch-based reclamation,
  RCU).
- **Memory reclamation:** in a lock-free structure you may not `free` a node
  another thread might still be reading.
- **Memory order arguments.** `<stdatomic.h>` has `_explicit` variants such as
  `atomic_load_explicit(&x, memory_order_relaxed)`. The order is a hint to the
  compiler about the synchronisation model. It is **not** the CPU's memory
  ordering or cache behaviour. With GCC on x86-64 and arm64 the generated code
  for load, store and `atomic_fetch_add` is the same for every value. Use the
  plain forms (`atomic_load`, `atomic_store`), which pick the safest order.
- Lock-free is not automatically faster. Under contention a CAS retry loop can
  spin, while a mutex sleeps. What it does remove is blocking on a
  lower-priority thread, which is why it matters for real-time work.

## Load and interference

| Source | How to create it | What it does |
|---|---|---|
| CPU | `stress-ng --cpu 2 --timeout 30` | competes for CPU with same- or lower-policy tasks |
| I/O | `stress-ng --io 1 --timeout 30` | interrupts and softirq work, lock contention in the kernel |
| Memory | `stress-ng --vm 1 --vm-bytes 256M --timeout 30` | page faults and reclaim; `mlockall` protects you |
| Scheduler | `hackbench -l 1000` | many wake-ups and context switches |

Always add `--timeout` on the shared machine.

## Tools

| Tool | Use |
|---|---|
| `cyclictest --mlockall --priority=80 --interval=1000 --duration=10s` | reference wake-up latency |
| `pi_stress --duration 10` | exercises priority-inheritance mutexes |
| `chrt`, `taskset`, `tuna` | set or inspect policy, priority, affinity |
| `ps -eLo pid,tid,class,rtprio,psr,comm` | per-thread scheduling state |
| `perf stat`, `perf sched`, `perf record` | counters, scheduler latency, profiles |
| `sudo trace-cmd record -e sched_switch -e sched_wakeup ./prog` | kernel scheduling trace (the only sudo allowed) |
| `valgrind --tool=helgrind`, `-fsanitize=thread` | data races and lock-order problems |
| `-fsanitize=address` | use-after-free (Lab 7 ABA demo) |

## Limits of the lab environment

- The lab machine is a virtual machine. It measures scheduling and kernel
  latency well. It can't show real device interrupt latency, and absolute
  numbers are worse than on bare metal.
- About twenty students share its CPUs, so compare your own runs with each
  other, not with published figures.
- The local Docker image uses the host's kernel, which is not `PREEMPT_RT`.
  Use it to build and check behaviour, not to measure.
