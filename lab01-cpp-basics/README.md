# Lab 1 — C++ and POSIX basics for real-time programs

The tools every later lab uses: high-resolution clocks, threads, storing
measurements, command-line arguments, busy work, scheduling policies and the
statistics that describe latency. Six short programs, one idea each.

Time: about 90 minutes. No sudo needed anywhere in this lab.

---

## 1. Build

```
cd rt-labs/lab01-cpp-basics
git pull
make
```

Each program is one `.cpp` file. Read the file before you run it; the comment
at the top says what it does and how to call it.

## 2. Clocks: `time_basics`

```
./time_basics
```

`CLOCK_MONOTONIC` counts from boot and never jumps. A `timespec` is whole
seconds plus nanoseconds, and the difference of two readings is the only
number that means anything. Part 3 of the output shows that every sleep ends
late, never early:

```
3. requested vs measured sleep
   target ms | measured ms | late by us
           1 |       1.935 |      935.1
           5 |       7.242 |     2241.5
          10 |      10.568 |      568.0
```

Output from the local Docker image (replace with lab-machine output). On the
lab machine the lateness is a few tens of microseconds.

## 3. Threads: `threads`

```
./threads
```

The same work runs once with `pthread_create` and once with `std::thread`.
Each thread gets its own `Work` struct, so no locking is needed. Remember
two things for later labs:

- pthread functions return an error number. They do not set `errno`, so
  `perror()` after them prints the wrong message. Use `strerror(rc)`.
- `std::thread` cannot be given a scheduling policy before it starts. That is
  why real-time threads in these labs are created with pthreads.

## 4. Measurements to a file: `file_io`

```
./file_io
cat measurements.txt
```

The program collects everything in a `std::vector` first and writes the file
afterwards. File I/O inside a timing loop would disturb what you measure.

## 5. Arguments and busy work: `busy_wait`

```
./busy_wait            # 100 ms, 3 times
./busy_wait 20 10      # 20 ms, 10 times
./busy_wait abc        # rejected
```

Open `htop` in a second tmux pane (`Ctrl+b %`) while `./busy_wait 2000 5`
runs and find the CPU at 100 %. Later labs use exactly this kind of loop to
stand in for real computation.

## 6. Scheduling policies: `scheduling`

```
./scheduling           # SCHED_FIFO 50, then back to SCHED_OTHER
./scheduling 80
./scheduling 90
```

```
2. RLIMIT_RTPRIO: soft 80, hard 80  (same as 'ulimit -r')
   now:     SCHED_OTHER, priority 0

3. switch to SCHED_FIFO 90
   failed: Operation not permitted
   priority 90 is above your limit of 80
```

No sudo, no `setcap`, no special container flags. The administrator gave your
account `rtprio 80` in `/etc/security/limits.d/`, and the kernel checks every
request against it.

## 7. Statistics: `statistics`

```
./statistics
./statistics 42        # different random data
```

The simulated data has rare spikes. Compare the mean with `max` and `p99.9`.
Also check that the histogram percentages add up to 100: the buckets are
half-open, `[from, to)`, so no sample is counted twice.

## 8. Experiments

**A. Sleep lateness under load.** In a second pane run
`stress-ng --cpu 2 --timeout 30`, and repeat `./time_basics` while it runs.

**B. Timer slack.** Normal processes get 50 us of timer slack
(`man 2 prctl`, `PR_SET_TIMERSLACK`). Run `chrt -f 50 ./time_basics`: the
same program at a real-time priority. What changed in part 3?

**C. Break the struct pattern.** In `threads.cpp`, make all three workers
write into the same `Work` object. Run it several times. Is the result always
the same? Now build with `make CXXFLAGS="-std=c++17 -O1 -g -pthread -fsanitize=thread" threads -B`
and run it again.

**D. Percentiles.** Change the spike probability in `statistics.cpp` from
0.5 % to 2 %. Which of min, mean, p90, p99, p99.9 and max change, and why?

**E. Stretch: a CSV from a real measurement.** Change `file_io.cpp` to take
1000 samples of a 1 ms `nanosleep` and store the lateness. Keep the output
format, because Lab 2 plots files like this.

## 9. Questions

1. Why is `CLOCK_REALTIME` wrong for measuring an interval?
2. `nanosleep(1 ms)` can take longer than 1 ms, but never less. Why never less?
3. What does `volatile` do in `busy_wait`, and what happens at `-O2` without it?
4. `pthread_setschedparam` returned `EPERM`. Which limit caused it, and how do
   you see its value?
5. For a 1 kHz control loop, which statistic from `statistics` would you put
   in a requirement: mean, p99 or max? Explain.

## House rules

- A load generator always gets a `--timeout`.
- `pkill -u $USER busy_wait` if something keeps running.
- Numbers on the shared machine are for comparing your own runs, not benchmarks.
