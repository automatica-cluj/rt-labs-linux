# Linux scheduling policies for real-time work

A reference for the labs. It describes the policies as they behave on the lab
machine: Debian 13, Linux 6.12 with `PREEMPT_RT`, and a normal user account
limited to real-time priority 80.

## The policies

| Policy | Priority | Picks the next task by | Typical use |
|---|---|---|---|
| `SCHED_OTHER` | 0 (nice -20..19 sets a weight) | fairness (EEVDF since 6.6) | almost everything |
| `SCHED_BATCH` | 0 | like OTHER, fewer preemptions | long CPU-bound jobs |
| `SCHED_IDLE` | 0 | only when nothing else wants the CPU | background work |
| `SCHED_FIFO` | 1..99 | highest priority, first come first served | real-time tasks |
| `SCHED_RR` | 1..99 | like FIFO, with time slices among equal priorities | real-time tasks that share a priority |
| `SCHED_DEADLINE` | none (runtime, deadline, period) | earliest deadline first | reservations; needs `sched_setattr` |

Every task with a real-time policy runs before any `SCHED_OTHER` task on the
same CPU. Among real-time tasks, a higher number means a higher priority.
`SCHED_DEADLINE` tasks run before all of them.

### SCHED_FIFO

- A running FIFO task keeps the CPU until it blocks (sleeps, waits for a lock
  or I/O), yields, or a task of *higher* priority becomes ready.
- Equal priorities do not preempt each other. A new arrival waits at the tail
  of its priority's queue.
- There is no time slice, so a FIFO task that never blocks starves everything
  below it on that CPU. The only exception is RT throttling (see below).

### SCHED_RR

- The same as FIFO, except that tasks of *equal* priority take turns. Each one
  runs for a time slice and then moves to the tail of its queue.
- The slice is `/proc/sys/kernel/sched_rr_timeslice_ms` (100 ms by default).
  `sched_rr_get_interval()` returns it.
- A higher-priority task still preempts at once, and a lower-priority task
  still waits.

### SCHED_OTHER

- The default. Since Linux 6.6 it is implemented by EEVDF, which replaced CFS.
- Its priority is always 0. `nice` changes the share of CPU time, not the order.
- Good at giving a mostly sleeping task a quick wake-up. It makes no promise
  about the worst case.

## Rules that matter on the lab machine

**Who may use a real-time policy.** A process needs either `CAP_SYS_NICE` or a
high enough `RLIMIT_RTPRIO`. Student accounts get `rtprio 80` from
`/etc/security/limits.d/`, so priorities 1..80 work without sudo and 81..99
fail with `EPERM`. Check your limit with `ulimit -r`. Lowering your own
priority, or returning to `SCHED_OTHER`, is always allowed.

**Memory locking.** `mlockall()` needs a large enough `RLIMIT_MEMLOCK`
(`ulimit -l`). Student accounts can lock 256 MiB.

**RT throttling.** `/proc/sys/kernel/sched_rt_runtime_us` (950000) out of
`sched_rt_period_us` (1000000) caps how much of each second real-time tasks
may use on a CPU. The remaining 5 % stays available for normal tasks, so a
runaway `while (true) {}` at priority 80 cannot freeze the machine. Since
Linux 6.12 the same guarantee comes from the *fair server*, a
`SCHED_DEADLINE` reservation that runs `SCHED_OTHER` tasks for about 50 ms per
second on a CPU where real-time tasks would otherwise starve them. In both
cases a pure CPU-bound real-time task can be interrupted for up to 50 ms. Real
real-time tasks block long before that.

**Kernel threads.** On a `PREEMPT_RT` kernel most interrupt handlers run as
threads at `SCHED_FIFO 50`. A priority-80 task can preempt them. That lowers
your latency, but it also delays the network and disk for everyone. That's one
more reason to keep real-time tasks short and blocking.

**CPUs.** Scheduling decisions are per CPU. On an 8-CPU machine, three threads
at different priorities usually run at the same time on three CPUs, and
priority has no visible effect. To study priorities, pin the threads to one
CPU. The labs pick that CPU from your user id (`rt::default_cpu()`) so
students don't all share CPU 0. Your account also has a CPU quota of two CPUs'
worth of time (`CPUQuota=200%`).

## API

The labs use the pthread calls. They act on one thread and return an error
number instead of setting `errno`.

```cpp
// Change the calling thread.
sched_param sp{};
sp.sched_priority = 50;
int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
if (rc != 0) fprintf(stderr, "pthread_setschedparam: %s\n", strerror(rc));

// Read it back.
int policy;
pthread_getschedparam(pthread_self(), &policy, &sp);
```

To start a thread that is real-time from its first instruction, set the
attributes *and* `PTHREAD_EXPLICIT_SCHED`. Without that line the attributes
are silently ignored and the thread inherits the creator's policy:

```cpp
pthread_attr_t attr;
pthread_attr_init(&attr);
pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);  // essential
pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
sched_param sp{};
sp.sched_priority = 60;
pthread_attr_setschedparam(&attr, &sp);

cpu_set_t cpus;                      // optional: pin to CPU 2
CPU_ZERO(&cpus);
CPU_SET(2, &cpus);
pthread_attr_setaffinity_np(&attr, sizeof(cpus), &cpus);

int rc = pthread_create(&tid, &attr, fn, arg);   // EPERM if 60 > ulimit -r
pthread_attr_destroy(&attr);
```

`rt::start_thread()` in `common/rt.hpp` does exactly this.

`sched_setscheduler(pid, ...)` is the older process-level call. On Linux it
also changes a single thread, which makes it easy to misuse from threaded
code. `std::thread` cannot set a policy before the thread starts, and
`std::mutex` has no priority inheritance. Use pthreads for both.

## Command line

```
chrt -f 50 ./prog                 # start prog as SCHED_FIFO 50
chrt -r 50 ./prog                 # SCHED_RR 50
chrt -p <pid>                     # show a process's policy
chrt -m                           # show priority ranges (works for anyone)
chrt -f 1 true && echo "RT allowed"   # a real permission check
taskset -c 3 ./prog               # run on CPU 3 only
ps -eLo pid,tid,class,rtprio,psr,comm   # per thread: policy, priority, CPU
tuna show_threads                 # the same, with more detail
ulimit -r; ulimit -l              # your RT priority and memlock limits
cat /proc/sys/kernel/sched_rt_runtime_us
```

`ps -eo ...` without `L` shows only each process's main thread, and that
thread is often not the real-time one.

## Common mistakes

1. **Attributes without `PTHREAD_EXPLICIT_SCHED`.** The thread silently runs as
   `SCHED_OTHER`.
2. **Ignoring the return value.** On `EPERM` the program carries on as a normal
   process and reports results that look plausible but mean nothing. Stop
   instead.
3. **`perror` after a pthread call.** Those calls return the error and leave
   `errno` alone, so `perror` prints a wrong or stale message. Use `strerror(rc)`.
4. **Priority 99.** It is reserved in practice for kernel watchdogs and
   migration threads, and it is above the student limit anyway.
5. **Busy-waiting at real-time priority.** It starves everyone below you on that
   CPU until throttling steps in. Block instead: sleep until an absolute time,
   or wait on a condition variable.
6. **Unpinned priority experiments.** On a multi-core machine, priorities only
   compete when threads share a CPU.
7. **More than 100 % utilisation.** No policy helps if the sum of `C/T` over
   your periodic tasks on a CPU exceeds 1. Rate-monotonic scheduling already
   guarantees less: 3 tasks are guaranteed schedulable only up to 78 %.

## Man pages

`sched(7)`, `pthread_setschedparam(3)`, `pthread_attr_setinheritsched(3)`,
`sched_setattr(2)`, `chrt(1)`, `taskset(1)`, `getrlimit(2)`,
`limits.conf(5)`, `mlockall(2)`, `clock_nanosleep(2)`.
