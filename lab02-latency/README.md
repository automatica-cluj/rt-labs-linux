# Lab 2 — Latency and jitter

Measure how late a periodic task wakes up, see what a real-time priority and
system load do to that number, find out why relative sleeps drift, and check
your tool against `cyclictest`.

Time: about 90 minutes. No sudo needed anywhere in this lab.

---

## 1. Build

```
cd rt-labs/lab02-latency
git pull
make
```

Read `latency.cpp`, and read `../common/rt.hpp` once. From this lab on, the
steps from Lab 0 (`mlockall`, `pthread_setschedparam`, absolute
`clock_nanosleep`) live in that header so each program can focus on its topic.

## 2. Run

```
./latency            # SCHED_FIFO 80, 5 s, absolute sleep, 1 ms period
```

```
latency: SCHED_FIFO priority 80, CPU 0
mode abs, period 1000 us, 3000 iterations
wake-up latency over 3000 samples: min 288 us, avg 1971 us, max 4167 us
jitter (max - min): 3878 us
histogram:
      0 ..    10 us         0
     ...
drift: 3000 periods should take 3000.000 ms, took 3003.505 ms (+3.505 ms)
```

Output from the local Docker image (replace with lab-machine output). Docker
Desktop runs a non-real-time kernel inside a VM, so its numbers are about a
hundred times worse than the lab machine's. The program is the same.

Arguments: `./latency [priority] [seconds] [mode] [period-us] [csv-file]`.
Priority 0 means `SCHED_OTHER`. Mode is `abs` or `rel`.

## 3. Experiments

Use `tmux` so you can have two panes (`Ctrl+b %`, `Ctrl+b o`).

**A. Normal versus real-time, idle**

```
./latency 0 10 abs 1000 other.csv
./latency 80 10 abs 1000 fifo.csv
```

Compare min, avg and max, and look at the histograms.

**B. Under load**

In the second pane:

```
stress-ng --cpu 2 --io 1 --timeout 60
```

While it runs:

```
./latency 0 10 abs 1000 other-stress.csv
./latency 80 10 abs 1000 fifo-stress.csv
```

Your account is limited to two CPUs' worth of time on the shared machine, so
`--cpu 2` is as much load as you can create yourself.

**C. Plot**

```
python3 plot_latency.py other.csv fifo.csv other-stress.csv fifo-stress.csv
```

The plot is written to `other.png`. The machine has no display; copy the file
to your computer with
`scp -P 2222 studentNN@193.226.6.123:rt-labs/lab02-latency/other.png .`.
Which series has the longest tail in the histogram?

**D. Absolute versus relative sleep**

```
./latency 80 5 abs
./latency 80 5 rel
```

Look at the `drift` line. With `rel`, each period starts when the previous
wake-up happened, so every bit of lateness pushes the rest of the schedule
back. The following output is from Docker, where each wake-up is about 1 ms
late:

```
mode rel, period 1000 us, 3000 iterations
drift: 3000 periods should take 3000.000 ms, took 5978.352 ms (+2978.352 ms)
```

**E. Cross-check with cyclictest**

```
cyclictest --mlockall --priority=80 --interval=1000 --duration=10s
```

Its `Max` should be close to your `max` from experiment A. The warning about
`/dev/cpu_dma_latency` is expected without root. With a histogram:

```
cyclictest --mlockall --priority=80 --interval=1000 --duration=10s --histogram=500 --quiet
```

## 4. Questions

1. Define latency and jitter using your numbers from experiment A.
2. In experiment B, which of min, avg and max changed most for priority 0? For
   priority 80? Why is max the number a real-time engineer cares about?
3. Explain the drift in experiment D. Why does `abs` mode not drift, even
   though its individual wake-ups are just as late?
4. The program never calls `printf` inside the loop, and writes the CSV after
   the loop. What would each of those do to the measurement?
5. `min` at priority 0 is noticeably higher than at priority 80 on an idle
   machine. Look up *timer slack* (`man 2 prctl`). Test your explanation with
   `chrt -f 1 ./latency 0 5`. What happens, and why?
6. A 1 kHz motor controller must never wake more than 200 us late. On the
   evidence you collected, could it run as a `SCHED_OTHER` process on this
   machine? As `SCHED_FIFO 80`? What more would you need to measure before
   you could be sure?

## House rules

- Load generators always get a `--timeout`.
- Don't run hour-long tests at priority 80. Ten seconds is plenty.
- `rm *.csv *.png` when you are done. Home directories are small.
