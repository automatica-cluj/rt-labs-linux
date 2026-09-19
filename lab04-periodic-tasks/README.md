# Lab 4 — Periodic tasks and deadlines

Most real-time software is a set of periodic tasks: read a sensor every 5 ms,
run a controller every 10 ms, log every 20 ms. In this lab you build intuition
for the task model (period, computation time, deadline), measure when a task
misses its deadline, and check a small Rate Monotonic system against the
schedulability theory.

Time: about 90 minutes. No sudo needed anywhere in this lab.

---

## 1. Get the code and build

```
cd rt-labs && git pull
cd lab04-periodic-tasks
make
```

Two programs:

| program | what it is |
|---|---|
| `periodic_task` | one periodic task, configurable period and computation time |
| `control_app` | three tasks (sensor, controller, logger) with Rate Monotonic priorities |

Both are plain C and use the helpers in `../common/rt.h` (introduced in lab 2):
`rt_lock_memory()`, `rt_set_self_sched()` / `rt_start_thread()`, and an
absolute `rt_sleep_until()`. Read `periodic_task.c` first. It is short, and the
loop reads top to bottom: wait for the release, measure the latency, do the
work, check the deadline, compute the next release.

## 2. Terms used below

- **release**: the moment a job of the task should start (every `period`)
- **release latency**: how late the job actually started
- **computation time (C)**: how long the job computes; the worst case is the WCET
- **response time (R)**: release to completion. The deadline is the next
  release, so the job **misses** when R > period
- **utilisation (U)**: C / period, summed over all tasks

**Overrun policy.** If a job finishes so late that later releases have already
passed, those releases are **skipped**: they are counted, not executed late,
and the task continues with the next release that is still in the future.
Without a policy like this, a late task fires a burst of back-to-back
catch-up jobs and makes everything worse.

Nothing is printed inside the periodic loop. `printf` to a terminal can
block, which would cause the very misses we are trying to measure. Results are
collected in memory and printed at the end.

## 3. One periodic task

```
./periodic_task [priority] [seconds] [cpu] [period_us] [wcet_us]
```

The defaults are priority 80, 5 s, a CPU chosen from your user id, a 10 ms
period and 2 ms of computation. `cpu -1` means do not pin, and priority 0
means SCHED_OTHER.

```
./periodic_task
```

Output from the local Docker image (replace with lab-machine output):

```
periodic_task: SCHED_FIFO priority 80, CPU 6
period 10000 us, computation 2000 us, utilisation 20.0 %, 5 s

jobs run 500, releases skipped 0
release latency over 500 samples: min 388 us, avg 979 us, max 2396 us
response time   over 500 samples: min 2389 us, avg 2980 us, max 4396 us
deadline misses: 0 of 500 jobs (0.00 %)
RESULT: all deadlines met (worst response 4396 us, slack 5603 us)
```

The Docker image runs on an ordinary kernel inside a virtual machine, so
its latencies are in milliseconds. On the PREEMPT_RT lab machine expect
tens of microseconds. What matters is how the numbers change between runs.

## 4. Three tasks under Rate Monotonic Scheduling

```
./control_app [priority] [seconds] [cpu] [load_percent] [reverse]
```

With priority P, the tasks get P, P-10 and P-20, from shortest to longest
period. All three are pinned to one CPU, because the theory below is about a
single CPU. Before running, the program prints the analysis:

- **Liu & Layland bound**: n tasks are always schedulable under RMS if
  U ≤ n(2^(1/n) − 1), which is 78 % for n = 3. The test is sufficient, not
  necessary: above the bound a task set may still be schedulable.
- **Response-time analysis (R bound)**: the exact worst-case response time of
  each task, R = C + Σ over higher-priority tasks ⌈R / T⌉ · C, iterated until
  it stops changing. If R ≤ period for every task, the set is schedulable.

```
./control_app
```

Output from the local Docker image (replace with lab-machine output):

```
task         period     wcet      U priority      R bound
sensor         5 ms   800 us  16.0%       80       800 us
controller    10 ms  2000 us  20.0%       70      2800 us
logger        20 ms  3000 us  15.0%       60      6600 us
total utilisation 51.0 %, Liu & Layland bound for 3 tasks 78.0 %: schedulable by the bound

running on CPU 6 for 3 s
task         jobs  misses skipped   max latency  avg response  max response
sensor        600       0       0       1589 us       1777 us       2390 us
controller    300       0       0       2391 us       4084 us       4400 us
logger        150       0       0       4380 us       7132 us       7380 us
logger's last snapshot: sensor 597, output 1194 (600 sensor updates)
RESULT: all deadlines met
```

Compare the measured `max response` column with the predicted `R bound`.
The lower-priority tasks have a larger release latency. That is not a fault:
they are waiting for higher-priority jobs released at the same instant.

The three tasks share data through a pthread mutex created with
`rt_mutex_init(&plant_lock, 1)`, that is, with priority inheritance. Each task
holds it only long enough to copy a value in or out. Lab 5 shows why a plain
mutex would be a problem.

## 5. Experiments

Use `tmux` (`Ctrl+b %` splits the window) so that you can run a load
generator next to the program.

**A. Normal versus real-time**

```
./periodic_task 0
./periodic_task 80
```

Compare `max` release latency and worst response time. Then repeat both while
the other pane runs `stress-ng --cpu 2 --timeout 30`. Which one still meets all
deadlines?

**B. Find the breaking point of one task**

Keep the 10 ms period and increase the computation time:

```
./periodic_task 80 5 -1 10000 5000
./periodic_task 80 5 -1 10000 8000
./periodic_task 80 5 -1 10000 9000
./periodic_task 80 5 -1 10000 9500
```

Record the smallest WCET where misses appear, idle and under
`stress-ng --cpu 2 --timeout 60`. With 9 ms of computation, the Docker image
already missed a few (locally, 15 misses out of 485 jobs, 15 releases
skipped). Why is the breaking point below 10 ms, and why is `skipped` equal to
`misses` here?

**C. Load the Rate Monotonic system**

`load_percent` scales every computation time. Keep an eye on U, the bound,
and the R-bound column:

```
./control_app 80 5 -1 100
./control_app 80 5 -1 150     # U = 76.5 %, just under the bound
./control_app 80 5 -1 180     # U = 91.8 %, above the bound: what does R bound say?
./control_app 80 5 -1 200     # U = 102 %
```

Note: `-1` for the cpu here means *do not pin*. Run each once more with your
own CPU number (the one printed by `./periodic_task`) in place of `-1`. Which
of the two runs agrees with the analysis, and why?

**D. Break Rate Monotonic on purpose**

`reverse = 1` gives the longest period the highest priority:

```
./control_app 80 5 <cpu> 150 0
./control_app 80 5 <cpu> 150 1
```

In the Docker image, the reversed run gave the sensor 251 misses in 5 s,
while the RMS run was clean. U is identical in both runs. Explain the
difference using the R-bound column.

**E. Stretch: stop early**

Start `./control_app 80 60` and press `Ctrl+C` after a few seconds. The
statistics still make sense, because they are divided by the jobs that
actually ran. Read how `sigaction` and a `volatile sig_atomic_t` flag make this safe,
and why the signal handler does nothing else.

## 6. Questions

1. The periodic loop computes the next release as `release += period`, not
   `now + period`. What drifts in the second version over one hour?
2. What is the difference between release latency and response time? Which
   one decides whether a deadline is missed?
3. In experiment B the breaking point was below the period. Name two sources
   of the missing time.
4. The Liu & Layland test said "inconclusive" in experiment C at U = 91.8 %.
   What did the response-time analysis say, and which result does the
   measurement support?
5. Why do all tasks in `control_app` get the same CPU? What happens to the
   analysis when they are not pinned?
6. The overrun policy skips missed releases. Give one application where
   skipping is right, and one where every job must run even if late.

## House rules

- Real-time tasks at priority 80 run ahead of everyone else's shells. Keep
  runs short: `seconds` is 5 by default for a reason.
- Load generators always get a `--timeout`. Your account is capped at two
  CPUs, so `stress-ng --cpu 2` is enough.
- If something hangs, `pkill periodic_task` or `pkill control_app`.
