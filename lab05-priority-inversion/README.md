# Lab 5 — Priority inversion

A high-priority task that shares a mutex with a low-priority task can end up
waiting for a *medium*-priority task that never touches the mutex. In this lab
you make that happen on purpose, measure how long the high-priority task is
blocked, and then remove the problem with a priority inheritance mutex.

Time: about 90 minutes. No sudo needed, except for the optional `trace-cmd`
experiment.

Prerequisites: lab 0 (`check_env.sh` all OK) and the `common/rt.h` helpers
from lab 2.

Theory behind this lab (concept pages, in Romanian):
[Excludere mutuală](https://github.com/automatica-cluj/rt-concepts/blob/main/5-shared-resources/mutual-exclusion.md),
[Inversiunea de prioritate](https://github.com/automatica-cluj/rt-concepts/blob/main/5-shared-resources/priority-inversion.md).

---

## 1. Background

Three tasks, one CPU, one mutex:

1. LOW (priority 20) locks the mutex and starts working inside it.
2. HIGH (priority 60) wakes up, needs the mutex and blocks. That is expected:
   it has to wait for LOW to finish the critical section.
3. MEDIUM (priority 40) wakes up. It does not need the mutex, but it has a
   higher priority than LOW, so it preempts LOW.

Now HIGH waits for LOW, and LOW waits for MEDIUM. HIGH is effectively running
at MEDIUM's priority. This is **priority inversion**. How long it lasts depends
on how long MEDIUM (or any number of medium-priority tasks) keeps running, so
the blocking time has no upper bound: **unbounded priority inversion**.

**Priority inheritance** fixes it. While HIGH waits for a mutex that LOW
holds, the kernel runs LOW at HIGH's priority. MEDIUM can no longer preempt
LOW, LOW finishes the critical section, releases the mutex and drops back to
priority 20, and HIGH runs. HIGH now waits at most for one critical section:
**bounded priority inversion**.

On Linux you get this with one attribute:

```c
pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
```

`rt_mutex_init(&mutex, 1)` in `common/rt.h` does exactly that; with `0` you
get a default pthread mutex, which has no priority inheritance. After the
initialisation the program uses the normal `pthread_mutex_lock` and
`pthread_mutex_unlock` calls, so the critical section is easy to find in the
code.

A real case: in 1997 the Mars Pathfinder lander kept resetting itself a few
days after landing. A high-priority bus management task was blocked on a
mutex held by a low-priority meteorological task, which was in turn preempted
by medium-priority communication tasks, and a watchdog reset the system. The
VxWorks mutex had a priority inheritance option that was switched off. JPL
enabled it by uploading a patch to the lander, and the resets stopped.

## 2. Build

```
cd rt-labs/lab05-priority-inversion
make
```

Read `inversion.c` now, starting with `low_task`, `high_task` and
`medium_task`. The important parts:

- `rt_start_thread()` creates each thread as `SCHED_FIFO` at its priority and
  pinned to one CPU *before* it runs. If the priority is refused the program
  stops, instead of quietly running a meaningless experiment as `SCHED_OTHER`.
- LOW and MEDIUM measure their work in **CPU time**
  (`CLOCK_THREAD_CPUTIME_ID`), not wall-clock time. If LOW is preempted, its
  critical section really gets longer, like real work would.
- The order LOW locks, then HIGH requests, then MEDIUM starts is enforced with
  semaphores, not with sleeps, so every run follows the same script.
- Threads only record timestamps. Everything is printed after they finish,
  because `printf` from inside the threads would change the timing.

Usage:

```
./inversion [none|inherit] [cpu] [medium_ms] [cs_ms]
```

| argument | meaning | default |
|---|---|---|
| `none` / `inherit` | plain mutex / priority inheritance mutex | `none` |
| `cpu` | the CPU all three threads share, `-1` = no pinning | derived from your user id |
| `medium_ms` | CPU time MEDIUM burns | 200 |
| `cs_ms` | CPU time LOW needs inside the mutex | 50 |

The default CPU differs between accounts so that students on the shared
machine do not all measure each other on CPU 0.

## 3. Run it

```
./inversion none
```

Output from the local Docker image (replace with lab-machine output):

```
mutex protocol: PTHREAD_PRIO_NONE
all threads pinned to CPU 6
LOW prio 20 needs 50 ms inside the mutex, MEDIUM prio 40 burns 200 ms, HIGH prio 60

timeline (ms since start)
      0.6  LOW    locks the mutex                 (CPU 6)
      1.6  HIGH   requests the mutex and blocks   (CPU 6)
      4.3  MEDIUM starts running                  (CPU 6)
    204.4  MEDIUM finishes
    250.8  LOW    unlocks
    250.9  HIGH   gets the mutex

LOW's effective priority while HIGH waited: 20

HIGH blocked for 249.3 ms (LOW's whole critical section is 50 ms)
MEDIUM ran for 200.1 ms of that time
UNBOUNDED: HIGH waited 249.3 ms, longer than the 50 ms critical section, because MEDIUM ran in between
```

Follow the timeline. HIGH asked for the mutex after about 1 ms, when LOW still
needed about 49 ms. HIGH got it after 249 ms: the rest of LOW's critical
section plus all 200 ms of MEDIUM.

Now the same scenario with priority inheritance:

```
./inversion inherit
```

Output from the local Docker image (replace with lab-machine output):

```
mutex protocol: PTHREAD_PRIO_INHERIT
all threads pinned to CPU 6
LOW prio 20 needs 50 ms inside the mutex, MEDIUM prio 40 burns 200 ms, HIGH prio 60

timeline (ms since start)
      0.4  LOW    locks the mutex                 (CPU 6)
      0.9  HIGH   requests the mutex and blocks   (CPU 6)
     50.5  LOW    unlocks
     50.5  HIGH   gets the mutex
     55.5  MEDIUM starts running                  (CPU 6)
    255.5  MEDIUM finishes

LOW's effective priority while HIGH waited: 60

HIGH blocked for 49.6 ms (LOW's whole critical section is 50 ms)
MEDIUM ran for 0.0 ms of that time
BOUNDED: HIGH waited 49.6 ms, no longer than the 50 ms critical section
```

MEDIUM was created at the same moment as before, but it could not even start
until HIGH had finished: LOW ran at the inherited priority 60 while HIGH
waited. The program reads that effective priority from
`/proc/thread-self/stat`, because `pthread_getschedparam()` only reports the
priority a thread set for itself, never an inherited one.

## 4. Experiments

Use `tmux` (lab 0) if you want a second pane.

**A. Repeatability**

Run each version five times:

```
for i in 1 2 3 4 5; do ./inversion none | tail -1; ./inversion inherit | tail -1; done
```

The numbers should barely move. A demonstration that only works sometimes
teaches nothing, which is why the order of events is enforced with semaphores.
On the shared machine other students' load can add a few milliseconds.

**B. Why pinning matters**

```
./inversion none -1
```

Output from the local Docker image (replace with lab-machine output):

```
timeline (ms since start)
      0.6  LOW    locks the mutex                 (CPU 2)
      1.1  HIGH   requests the mutex and blocks   (CPU 5)
      3.5  MEDIUM starts running                  (CPU 5)
     50.6  LOW    unlocks
     50.6  HIGH   gets the mutex
    203.5  MEDIUM finishes
...
BOUNDED: HIGH waited 49.4 ms, no longer than the 50 ms critical section
```

Without pinning, MEDIUM ran on a different CPU from LOW, and the plain mutex
looks harmless. Does that mean priority inversion is not a problem on
multicore machines? Think about what happens when there are more
medium-priority tasks than free CPUs, or when a system pins tasks to CPUs
deliberately, as most real-time systems do.

**C. Blocking time grows with MEDIUM's work**

Pick one CPU, then vary MEDIUM's work from 0 to 400 ms with both protocols:

```
for m in 0 50 100 200 400; do ./inversion none 3 $m | grep '^HIGH blocked'; done
for m in 0 50 100 200 400; do ./inversion inherit 3 $m | grep '^HIGH blocked'; done
```

On the local Docker image the plain mutex gave 50, 100, 150, 250 and 450 ms,
and priority inheritance gave 50 ms every time. Draw both lines on one graph
(blocking time against `medium_ms`). Then keep MEDIUM at 200 ms and vary the
critical section (`cs_ms` = 10, 50, 100, 200) with `inherit`. What does the
blocking time depend on now?

**D. The kernel's own stress test**

`pi_stress` from rt-tests runs many groups of three threads in exactly this
low, medium and high pattern, using priority inheritance mutexes, and checks
that no inversion ever gets stuck:

```
pi_stress --duration 10 --groups 2
```

It prints a running count of inversions and stops after 10 seconds. If
priority inheritance were broken, a group would deadlock and the test would
report a failure.

**E. Watch the priority change in the scheduler trace (optional, sudo)**

On the lab machine `sudo trace-cmd` is the one allowed sudo command. Record
scheduler events while the program runs:

```
sudo trace-cmd record -e sched_switch -e sched_pi_setprio ./inversion inherit
trace-cmd report | grep -E 'sched_pi_setprio|inversion' | head -40
```

Find the `sched_pi_setprio` line where LOW's priority is raised, and the one
where it is lowered again. Compare the timestamps with the program's timeline.
Then record `./inversion none` and find the `sched_switch` events where MEDIUM
preempts LOW. (`trace-cmd` reports kernel priorities, where a lower number
means a higher priority. The kernel's priority equals 99 minus the
`SCHED_FIFO` priority.)

## 5. Questions

Write your answers in a short text file.

1. With the plain mutex, what is the worst case for HIGH's blocking time if
   there are *n* medium-priority tasks, each running for *C* ms? With
   priority inheritance?
2. In experiment B the plain mutex looked fine. Describe a two-CPU scenario
   where priority inversion still happens.
3. Priority inheritance bounds the blocking time by the length of the critical
   section. What does that tell you about how long critical sections in
   real-time code should be?
4. LOW is boosted to 60 while HIGH waits. What would happen if LOW, while
   boosted, blocked on a *second* mutex held by yet another low-priority task?
   Look up *transitive* (chained) priority inheritance.
5. The priority ceiling protocol (`PTHREAD_PRIO_PROTECT`) is an alternative to
   inheritance. How does it work, and what problem does it prevent that
   inheritance does not? (Hint: lab 6.)
6. Why does the program measure LOW's and MEDIUM's work in thread CPU time
   instead of wall-clock time? What would the plain-mutex run show if LOW's
   critical section was "busy for 50 ms of wall-clock time"?

## House rules

- The program runs three real-time threads for well under a second, far
  below the 95 % real-time throttling limit. If you make `medium_ms` and
  `cs_ms` much larger in your own copy, keep the total under a second.
- Use your default CPU, or pick one with `htop` that nobody else is loading.
- `pkill inversion` if something hangs. Nothing in this lab should.
