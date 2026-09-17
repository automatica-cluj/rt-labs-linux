# Lab 6 — Deadlock

Two tasks, two mutexes, opposite lock order. You will watch a deadlock happen,
detect it with a timed lock, and then prevent it in three different ways.

Time: about 90 minutes. No sudo needed anywhere in this lab.

---

## 1. Background

A **deadlock** is a set of tasks that each wait for something another task in
the set holds. None of them can ever continue. Four conditions (Coffman, 1971)
must all hold at the same time:

| Condition | Meaning here | How to break it |
|---|---|---|
| Mutual exclusion | a mutex has one owner at a time | share the resource (rarely possible) |
| Hold and wait | a task holds m1 while waiting for m2 | take all locks at once, or release and retry (`trylock`) |
| No preemption | nobody can take a mutex away from its owner | a timeout that makes the owner give it back |
| Circular wait | A waits for B, B waits for A | a global lock order (`ordered`) |

Remove any one condition and the deadlock cannot happen.

The scenario in both programs:

```
Task A: lock m1 ... lock m2
Task B: lock m2 ... lock m1      <- opposite order
```

## 2. Build

```
cd rt-labs/lab06-deadlock
make
```

`deadlock.cpp` shows the problem and timeout-based detection. `lock_order.cpp`
shows prevention. Both use the helpers in `../common/rt.hpp`.

## 3. Watch the deadlock

```
./deadlock 0
```

Timeout 0 means both tasks use a plain `pthread_mutex_lock`. They block forever.
A watchdog in `main` notices that nobody finished and ends the program, so your
terminal does not hang.

Output from the local Docker image (replace with lab-machine output):

```
timeout: none (plain pthread_mutex_lock), watchdog after 2100 ms
jitter: 0 ms, priority: 0

[    0.4 ms] task A: locking m1
[    0.4 ms] task A: got m1
[   55.4 ms] task B: locking m2
[   55.4 ms] task B: got m2
[  106.4 ms] task A: waiting for m2
[  159.7 ms] task B: waiting for m1

DEADLOCK: both threads stuck after 2108 ms
  m1 held by task A, m2 held by task B
```

The exit code is 3 (`echo $?`). A real system would have no watchdog printing a
friendly message: the two tasks would simply stop, and so would everything
waiting for them.

## 4. Detect it with a timeout

```
./deadlock
```

Now the second lock is `pthread_mutex_clocklock` with a 1000 ms deadline on
`CLOCK_MONOTONIC`. A task that times out assumes a deadlock, releases the mutex
it holds, backs off and tries again.

Output from the local Docker image (replace with lab-machine output):

```
timeout: 1000 ms (pthread_mutex_clocklock, CLOCK_MONOTONIC)
jitter: 0 ms, priority: 0

[    0.7 ms] task A: locking m1
[    0.7 ms] task A: got m1
[   51.0 ms] task B: locking m2
[   51.0 ms] task B: got m2
[  106.5 ms] task A: waiting for m2
[  155.9 ms] task B: waiting for m1
[ 1110.0 ms] task A: TIMEOUT, assuming deadlock, releasing m1
[ 1110.0 ms] task B: got m1
[ 1161.5 ms] task B: done, released both
[ 1315.6 ms] task A: locking m1
[ 1315.6 ms] task A: got m1
[ 1421.5 ms] task A: waiting for m2
[ 1421.6 ms] task A: got m2
[ 1476.3 ms] task A: done, released both

result
  task A: 2 attempt(s), 1 timeout(s), finished at 1476 ms
  task B: 1 attempt(s), 0 timeout(s), finished at 1162 ms
  resource1 = 11, resource2 = 11 (expected 11 and 11)
  deadlock detected 1 time(s) by timeout and recovered
```

Read the timeline carefully. It is the same on every run:

- A starts waiting at about 100 ms, so its deadline is about 1100 ms.
- B starts waiting at about 150 ms, so its deadline is about 1150 ms.
- A's deadline comes first. A gives up and releases m1.
- B is still waiting, gets m1 immediately and **never times out**.
- A backs off for 200 ms and succeeds on its second attempt.

Only one of the two tasks detects the deadlock, and one release is enough to
break the cycle. It also cost more than a second in which neither task did any
work.

Why `pthread_mutex_clocklock` and not the older `pthread_mutex_timedlock`?
`timedlock` measures its deadline on `CLOCK_REALTIME`, the wall clock, which
jumps when NTP or an administrator sets the time. `clocklock` (glibc 2.30+) lets
you use `CLOCK_MONOTONIC`, like every other timer in these labs.

## 5. Prevent it

```
./lock_order ordered
./lock_order trylock
./lock_order scoped
./lock_order wrong
```

| Mode | What task B does | Coffman condition removed |
|---|---|---|
| `ordered` | locks m1 then m2, the same global order as A | circular wait |
| `trylock` | locks m2, *tries* m1; if busy, releases m2, sleeps 5 ms, retries | hold and wait |
| `scoped` | `std::scoped_lock both(m2, m1);` | hold and wait (the library does the back-off) |
| `wrong` | locks m2 then m1 with plain locks | none: deadlock, watchdog, exit code 3 |

Output of `./lock_order ordered` from the local Docker image (replace with
lab-machine output):

```
mode: ordered, priority: 0

[   0.1 ms] task A: got m1
[  53.1 ms] task B: locking m1 then m2 (global order)
[ 105.1 ms] task A: locking m2
[ 105.1 ms] task A: has m1 and m2, working
[ 157.6 ms] task A: releasing both
[ 157.7 ms] task B: has m1 and m2, working
[ 210.4 ms] task B: released both

result
  task A: started   0.1 ms, held both from 105.1 ms, finished 157.6 ms, run time 157.6 ms
  task B: started  53.1 ms, held both from 157.7 ms, finished 210.4 ms, run time 157.3 ms
  total 219.6 ms, resource1 = 11, resource2 = 11 (expected 11 and 11)
  completed, no deadlock
```

B waits about 100 ms for A to finish, then does its own 50 ms of work.
Everything is over in about 210 ms, against about 1480 ms with detection and
recovery.

With `trylock` the result line also shows how many times B had to give up and
retry (7 or 8 in the Docker image).

## 6. Experiments

Open a second terminal or a `tmux` pane if you want to watch `htop` at the same
time.

**A. How short can the timeout be?**

```
./deadlock 500
./deadlock 100
./deadlock 20
```

For each run note when A's timeout fires and when the last task finishes. A
shorter timeout recovers sooner. Now look closely at the 20 ms run: when A
gives up at about 120 ms, is B already waiting for m1? Was there a cycle yet?
A timeout cannot tell "deadlocked" from "the other task is just slow", so a
short timeout reports deadlocks that do not exist.

**B. Make timing random**

The second argument adds a random 0..N ms delay before each
task's second lock:

```
./deadlock 150 200
./deadlock 150 200
./deadlock 150 200
```

Run it ten times. Note which task detects the deadlock each time, and whether
both ever time out. The two tasks still hold their first mutex for at least
100 ms, so they almost always overlap. Now change `kHoldMs` in `deadlock.cpp`
to 1, rebuild and repeat with `./deadlock 150 60`. How often is there no
deadlock at all? Programs with a latent deadlock often pass testing for exactly
this reason: the unlucky interleaving is rare.

**C. Real-time priorities do not help**

```
./deadlock 0 0 50
./lock_order ordered 50
```

Both tasks now run at `SCHED_FIFO` 50. The deadlock is just as dead. A priority
decides who runs, but it cannot make a blocked task runnable.

**D. Break the rule on purpose**

```
./lock_order wrong ; echo "exit code $?"
```

Then open `lock_order.cpp` and find the single place you would change to fix
the `wrong` mode.

**E. Stretch: three tasks**

Add a task C and a mutex m3 to `lock_order.cpp`:

- A locks m1 then m2
- B locks m2 then m3
- C locks m3 then m1

Is there a cycle? Show it with the watchdog. Then fix it with a global order
m1 < m2 < m3, and check that the fix works whichever task starts first.

## 7. Questions

Write your answers in a short text file. They will be discussed at the start
of the next lab.

1. In section 4, only task A timed out. Explain why from the two deadlines, and
   predict who times out if task B starts after 0 ms instead of 50 ms.
2. Which Coffman condition does each mode of `lock_order` remove? Why is
   removing a single condition enough?
3. A timeout detects a deadlock only after the timeout has passed. Why is that
   a bad fit for a hard real-time task? What else can a timeout detect by
   mistake?
4. Why does the program use `CLOCK_MONOTONIC` for the lock deadline? What could
   happen with `CLOCK_REALTIME` on a machine that synchronises its clock?
5. In `trylock` mode, could B retry forever? What is that situation called, and
   why do the two tasks in `deadlock.cpp` use different back-off times (200 ms
   and 300 ms)?
6. A global lock order is easy with two mutexes in one file. How would you keep
   it in a large program where locks are taken in different modules?

## House rules

- The watchdogs make both programs exit on their own. If you change the code and
  something hangs, `Ctrl+c` or `pkill deadlock` cleans up.
- Priorities above 0 are only useful for experiment C. Do not leave busy
  real-time loops running.
- Everything on the lab machine is wiped at the end of the term. Keep copies of
  your work.
