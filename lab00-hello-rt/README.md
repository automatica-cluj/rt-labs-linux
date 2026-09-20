# Lab 0 — Hello, real-time

First contact with the lab machine. By the end you will have logged in, checked
that your account can use real-time scheduling, built a small C program, and
seen what a real-time priority does to a periodic task.

Time: about 45 minutes. No sudo needed anywhere in this lab.

Theory behind this lab (concept pages, in Romanian):
[Ce înseamnă timp real](https://github.com/automatica-cluj/rt-concepts/blob/main/00-what-is-real-time.md),
[Ce este un sistem de operare de timp real](https://github.com/automatica-cluj/rt-concepts/blob/main/01-what-is-an-rtos.md),
[Latență și jitter](https://github.com/automatica-cluj/rt-concepts/blob/main/1-time/latency-and-jitter.md).

---

## 1. Log in

```
ssh -p 2222 studentNN@193.226.6.123
```

Use the account and password you were given. If SSH complains that the host
identification changed, the machine was rebuilt since you last connected. Run
`ssh-keygen -R '[193.226.6.123]:2222'` on your own computer and try again.

## 2. Get the labs

```
git clone https://github.com/automatica-cluj/ssatr-iaisc-icaf-2025-labs-v2.git rt-labs
cd rt-labs/lab00-hello-rt
```

Later labs are added to the same repository. `git pull` inside `rt-labs`
fetches them.

## 3. Check your environment

```
./check_env.sh
```

Every line should say `[OK]`. The important ones:

- the kernel line contains `PREEMPT_RT`, so the kernel is fully preemptible
- `ulimit -r` is `80`: you may use `SCHED_FIFO` priorities up to 80 without sudo
- `sched_rt_runtime_us` is `950000`: a runaway real-time task is throttled at
  95 % of a CPU instead of freezing the machine for everyone

If `ulimit -r` shows `0`, log out and back in. Limits are applied when the
session starts. If it is still 0, tell the instructor.

## 4. Build and run

```
make
./hello_rt
```

The program locks its memory, switches to `SCHED_FIFO` priority 80, then wakes
up every millisecond for 5 seconds and measures how late each wake-up was.
Read `hello_rt.c` now, it is short and every step is commented.

Output from the lab machine, idle:

```
running as SCHED_FIFO priority 80, period 1000 us, 5 s
wake-up latency over 5000 iterations:
  min      3 us
  avg     22 us
  max    116 us
```

Your numbers will differ. This is a shared virtual machine, so absolute values
mean little. What matters in this lab is how the numbers change between runs.

## 5. Experiments

Open a second terminal, or use `tmux` so both panes live in one SSH session
(`tmux`, then `Ctrl+b %` splits the window, `Ctrl+b o` switches panes).

**A. Normal versus real-time, idle machine**

```
./hello_rt 0      # SCHED_OTHER, an ordinary process
./hello_rt 80     # SCHED_FIFO 80
```

Compare all three lines. On the lab machine priority 0 gave roughly
min 60 / avg 95 / max 370 µs, priority 80 gave min 3 / avg 22 / max 116 µs.

**B. Normal versus real-time, loaded machine**

In the second pane put CPU load on your own account for 30 seconds:

```
stress-ng --cpu 8 --timeout 30
```

While it runs, in the first pane:

```
./hello_rt 0
./hello_rt 80
```

You will probably be surprised: priority 0 does not get much worse. A task that
sleeps 99 % of the time is exactly what the normal Linux scheduler is good at
waking quickly. Keep this in mind for the stretch exercise.

**C. The priority cap**

```
./hello_rt 90
```

Read the error message and the hint.

**D. Cross-check with the standard tool**

`cyclictest` from the `rt-tests` package does the same measurement, more
carefully, and is what people quote in papers and bug reports:

```
cyclictest --mlockall --priority=80 --interval=1000 --duration=10s
```

The warning about `/dev/cpu_dma_latency` is expected without root and
harmless. Compare its `Max` with yours from experiment A. `Ctrl+c` stops it
early.

**E. Stretch: make the task do work**

Real periodic tasks do not just wake up, they compute. Add about 300 µs of
busy work to every iteration, in `hello_rt.c`, right after the line that reads the clock into `now`:

```c
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        volatile double x = 1;
        do {
            x *= 1.0000001;
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while (diff_ns(now, t0) < 300000);
```

Rebuild and repeat experiment B. The reported latency now includes the work, so
anything above 1000 µs means the task missed its next deadline. On the lab
machine, under `stress-ng --cpu 8`:

| | avg | max |
|---|---|---|
| priority 0 | 690 µs | 4467 µs |
| priority 80 | 304 µs | 1669 µs |

Now the difference is not subtle. Remove the busy loop again, or keep a copy,
before the next lab.

## 6. Questions

Write your answers in a short text file. They will be discussed at the start
of the next lab.

1. In experiment A the `min` value is about 50 to 60 µs at priority 0 and
   about 3 µs at priority 80, on an idle machine. The scheduler is not busy,
   so what else is different? Look up *timer slack* (`man 2 prctl`,
   `PR_SET_TIMERSLACK`).
2. In experiment B, why did priority 0 barely suffer, and in experiment E, why
   did it suffer a lot? Use the words *preemption* and *priority*.
3. Why does the program call `mlockall()` before the loop? What could go
   wrong without it?
4. The loop uses `clock_nanosleep` with `TIMER_ABSTIME` instead of
   a relative sleep such as `usleep(1000)`. What would change over 5000 iterations
   with the relative version? (Hint: where is the next deadline computed from?)
5. Experiment C failed with `EPERM`. Which limit enforces this and why does a
   shared machine want such a limit?
6. `sched_rt_runtime_us` is 950000. What would happen to *other* students if
   it were -1 and you ran `while (true) {}` at priority 80?

## House rules

- Do not leave real-time processes running when you log off. `htop` shows
  what is yours; `pkill hello_rt` cleans up.
- Keep load tests short, as in `--timeout 30`. Your account is capped at two
  CPUs, but twenty people running `stress-ng` at once still hurts everyone.
- Everything on this machine is wiped at the end of the term. Keep copies of
  your work.
