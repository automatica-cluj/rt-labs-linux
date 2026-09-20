# Lab 3 — Scheduling policies

Linux gives you three scheduling policies: `SCHED_OTHER` (the normal, fair
scheduler), `SCHED_FIFO` and `SCHED_RR` (real-time). In this lab you watch them
decide who gets a CPU. You will see strict priority order, run-to-completion
versus time slicing, and the safety net that keeps a real-time task from taking
a CPU forever.

Time: about 90 minutes. No sudo needed anywhere in this lab.

Theory behind this lab (concept pages, in Romanian):
[Taskuri și stări](https://github.com/automatica-cluj/rt-concepts/blob/main/2-scheduling/tasks-and-states.md),
[Preempțiune cu priorități fixe](https://github.com/automatica-cluj/rt-concepts/blob/main/2-scheduling/fixed-priority-preemption.md),
[Înfometare și watchdog](https://github.com/automatica-cluj/rt-concepts/blob/main/2-scheduling/starvation-and-watchdogs.md).

---

## 1. Build

```
cd rt-labs/lab03-sched-policies
make
```

This builds three programs. They share `timeline.h` and `../common/rt.h`.

| Program | What it shows |
|---|---|
| `fifo_order [priority] [cpu] [work_ms]` | three workers at different priorities |
| `same_priority [fifo\|rr\|other] [priority] [cpu] [work_ms]` | three workers at the same priority |
| `policy_mix [priority] [cpu] [work_ms]` | one `SCHED_OTHER`, one `SCHED_RR` and one `SCHED_FIFO` worker |

All three use the same method:

1. Every worker burns a fixed amount of CPU time (`work_ms`, 150 ms by default)
   and records when it ran.
2. `main` runs one priority step above the workers, so none of them can start
   early. It releases them in a known order, then waits.
3. Each program prints a table and a timeline strip. In the strip, one column
   is a slice of time and the letter is the worker that had the CPU.

Read `timeline.h` first. Its top half is the real-time part, and the bottom
half only sorts and prints. Then read `rt_start_thread_policy()` and
`rt_lock_memory()` in `../common/rt.h`.

## 2. Why everything is pinned to one CPU

The lab machine has several CPUs. Three busy threads on a machine with eight
CPUs run in parallel, and no scheduling policy has anything to decide. A
policy only matters when tasks compete for the *same* CPU, so every worker is
pinned to one CPU.

The default CPU is derived from your user id, so twenty students do not all
land on CPU 0. The first line of output tells you which CPU you got. Pass a CPU
number to choose one yourself, or `-1` to switch pinning off.

## 3. Strict priority order

```
./fifo_order
```

The workers are released lowest priority first: Low, Medium, High. If release
order mattered, Low would win.

Output from the local Docker image (replace with lab-machine output):

```
main: SCHED_FIFO priority 71, CPU 6
3 workers x 150 ms of CPU time on CPU 6, released in order L M H

worker   requested     obtained       cpu  first ms   done ms pieces
L Low    FIFO 50       FIFO 50          6     300.1     450.1      1
M Medium FIFO 60       FIFO 60          6     150.1     300.1      1
H High   FIFO 70       FIFO 70          6       0.0     150.0      1

timeline, one column = 7.0 ms:
  |HHHHHHHHHHHHHHHHHHHHHMMMMMMMMMMMMMMMMMMMMMMLLLLLLLLLLLLLLLLLLLLL|
start order:  H M L
finish order: H M L
most pieces for one worker: 1
workers interleaved: no
result: finished in priority order, one after another
```

Check the `requested` and `obtained` columns. The program reads the policy back
from the kernel instead of trusting what it asked for. If a thread cannot get
its policy, the program stops with an error instead of quietly running as
`SCHED_OTHER`.

`pieces` counts how often a worker lost the CPU for more than 0.5 ms. Under
`SCHED_FIFO` each worker ran in one piece.

## 4. Experiments

Use `tmux` if you want two panes (`Ctrl+b %` splits, `Ctrl+b o` switches).

**A. The same program without real-time priorities**

```
./fifo_order 0
```

Output from the local Docker image (replace with lab-machine output):

```
worker   requested     obtained       cpu  first ms   done ms pieces
L Low    OTHER 0       OTHER 0          6       0.0     446.9     54
M Medium OTHER 0       OTHER 0          6       5.6     450.0     54
H High   OTHER 0       OTHER 0          6       2.8     448.5     54

timeline, one column = 7.0 ms:
  |HHLLMMHLLMMMHLLMMMHLLMMMHLLLMHHHLLMMHLLMMMHLLMLMMHHLLMHHLLLMHHHM|
```

The fair scheduler switches between the three about every 3 ms, and all of
them finish at about the same time. Nobody finished early.

**B. Equal priorities: FIFO, RR and OTHER**

```
./same_priority fifo
./same_priority rr
./same_priority other
```

Output of `rr` from the local Docker image (replace with lab-machine output):

```
SCHED_RR time slice: 100 ms
3 workers x 150 ms of CPU time, SCHED_RR priority 50, CPU 6

worker   requested     obtained       cpu  first ms   done ms pieces
A A      RR 50         RR 50            6       0.0     350.0      2
B B      RR 50         RR 50            6      99.4     400.1      2
C C      RR 50         RR 50            6     199.3     450.1      2

timeline, one column = 7.0 ms:
  |AAAAAAAAAAAAAABBBBBBBBBBBBBBCCCCCCCCCCCCCCCAAAAAAABBBBBBBCCCCCCC|
```

- With `fifo` the timeline is `AAA…BBB…CCC`. A FIFO task keeps the CPU until it
  blocks or finishes. Priority only decides between *different* priorities.
- With `rr` each worker gets one time slice (see
  `/proc/sys/kernel/sched_rr_timeslice_ms`), then goes to the back of the
  queue.
- With `other` the workers switch every few milliseconds, as in experiment A.

Run `./same_priority rr 50 -1 300` (unpinned, 300 ms of work). What changed,
and why? The note printed at the end tells you.

**C. Mixed policies, and who is waiting for whom**

```
./policy_mix
```

Output from the local Docker image (replace with lab-machine output):

```
3 workers x 150 ms of CPU time on CPU 6, released in order O R F

worker   requested     obtained       cpu  first ms   done ms pieces
O other  OTHER 0       OTHER 0          6     300.1     450.1      1
R rr     RR 50         RR 50            6       0.0     300.0      2
F fifo   FIFO 50       FIFO 50          6      99.4     249.4      1

timeline, one column = 7.0 ms:
  |RRRRRRRRRRRRRRFFFFFFFFFFFFFFFFFFFFFRRRRRRRROOOOOOOOOOOOOOOOOOOOO|
```

`O` was released first and did nothing until both real-time tasks were done.
`R` and `F` have the same priority. `R` was released first, so it started. After
one time slice it went to the back of the queue, behind `F`. `F` is FIFO, so it
was never sliced and ran to completion. Then `R` finished.

The program only runs for about half a second. To look at its threads while
it runs, start it in the background and call `ps` after 0.2 s:

```
./policy_mix > /dev/null & sleep 0.2; ps -eLo pid,tid,class,rtprio,psr,comm | grep -E 'PID|policy_mix'; wait
```

```
  PID   TID CLS RTPRIO PSR COMMAND
   53    53 FF      51   6 policy_mix
   53    55 TS       -   6 policy_mix
   53    56 RR      50   6 policy_mix
   53    57 FF      50   6 policy_mix
```

`-L` matters here. Without it `ps` shows only the main thread. `CLS` is the
policy (`TS` is `SCHED_OTHER`, `FF` is FIFO, `RR` is round-robin) and `PSR` is
the CPU.

**D. The priority limit, and what a failure looks like**

```
(ulimit -r 40; ./fifo_order 70)
```

```
pthread_setschedparam(priority 71): Operation not permitted
hint: 'ulimit -r' shows the highest real-time priority you may use
```

The parentheses start a subshell, so your own shell keeps its limit. Your
account may use priorities up to `ulimit -r` (80 on the lab machine). A thread
that cannot get its policy is a bug to fix, not a warning to ignore.

`chrt` and `taskset` do the same things from the command line, for programs you
cannot change:

```
chrt -m                          # priority ranges per policy (works without privileges)
chrt -f 30 ./same_priority other # start a program as SCHED_FIFO 30
taskset -c 2 ./fifo_order 0 -1   # allow only CPU 2
chrt -p <pid>                    # policy of a running process
```

**E. Stretch: the real-time safety net**

A real-time task that never blocks would starve everything else on its CPU,
including your shell. Linux reserves a little time for normal tasks:

```
cat /proc/sys/kernel/sched_rt_runtime_us /proc/sys/kernel/sched_rt_period_us
```

The values are 950000 and 1000000. In every second, real-time tasks may use at
most 950 ms of a CPU. Since Linux 6.12 this reservation is provided by the
*fair server*, a small deadline task that runs normal tasks when they have been
starved for about that long.

Give each worker 600 ms of work, so the real-time part lasts about 1.2 s
without a break:

```
./policy_mix 50 6 600     # use your own CPU number instead of 6
```

Your CPU number is on the first line of any earlier run.

Output from the local Docker image (replace with lab-machine output):

```
worker   requested     obtained       cpu  first ms   done ms pieces
O other  OTHER 0       OTHER 0          6     949.5    1800.1      2
R rr     RR 50         RR 50            6       0.0    1250.1      3
F fifo   FIFO 50       FIFO 50          6      99.5     699.5      1

timeline, one column = 28.1 ms:
  |RRRRFFFFFFFFFFFFFFFFFFFFFRRRRRRRRROORRRRRRRROOOOOOOOOOOOOOOOOOOO|
```

`O` got a first short piece at about 950 ms, while real-time work was still
pending. That is the safety net. Do not run this in a loop: on the shared
machine that CPU belongs to other students too.

## 5. Questions

Write your answers in a short text file. They will be discussed at the start
of the next lab.

1. In section 3 the workers were released in the order L, M, H but ran H, M, L.
   In experiment B (`fifo`) they were released A, B, C and ran A, B, C. What
   decides the order in each case?
2. Why does `main` run at one priority *above* the workers? Change
   `setup_main(prio + 1, cpu)` in `policy_mix.c` to `setup_main(0, cpu)`,
   rebuild, and explain what changes.
3. In experiment C `F` finished before `R`, although `R` started first and has
   the same priority. Explain it using the words *time slice* and *queue*.
4. When is `SCHED_RR` a better choice than `SCHED_FIFO`, and when is it worse?
5. `rt_start_thread_policy()` in `rt.h` calls
   `pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED)`. What happens
   to the policy and priority in the attributes if you remove that line? Try
   it: the `obtained` column will tell you.
6. On a `PREEMPT_RT` kernel most interrupt handlers run as kernel threads at
   `SCHED_FIFO` 50 (`ps -eLo pid,class,rtprio,comm | grep irq/`). What does a
   CPU-bound task at priority 60 do to network or disk interrupts on its CPU?
7. Experiment E: what would happen to the other users of that CPU if
   `sched_rt_runtime_us` were -1?

## House rules

- Real-time workers in this lab are short (about 0.5 s). Do not wrap them in
  endless loops, and do not raise `work_ms` beyond what an experiment asks for.
- Load generators always get a timeout: `stress-ng --cpu 2 --timeout 30`.
- Use your own CPU (the default) rather than CPU 0, which everyone else would
  pick.
- Clean up before you log off: `pkill -u $USER fifo_order`, and the same for
  the other programs. `htop` shows what is still yours.
