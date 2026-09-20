# Real-Time Systems — Labs

Lab material for the real-time systems course (SSATR / IAISC / ICAF). The
labs run on a shared Linux machine with a `PREEMPT_RT` kernel (Debian 13,
Linux 6.12). Each lab is a directory with its own README, plain C source code and
a Makefile.

## Connecting

```
ssh -p 2222 studentNN@193.226.6.123
```

The instructor gives you a personal account and password. If SSH warns that
the host key changed, the machine has been rebuilt. Run
`ssh-keygen -R '[193.226.6.123]:2222'` and connect again.

Once logged in:

```
git clone https://github.com/automatica-cluj/rt-labs-linux.git rt-labs
cd rt-labs
```

Run `git pull` here before each lab to get new material.

## Labs

| Lab | Topic | Week |
|---|---|---|
| [lab00-hello-rt](lab00-hello-rt/) | Log in, check the environment, a first periodic real-time task | 0 |
| [lab01-c-basics](lab01-c-basics/) | Clocks, threads, data files, arguments, busy work, policies, statistics | 0 |
| [lab02-latency](lab02-latency/) | Latency and jitter, load, absolute vs relative sleep, cyclictest | 1 |
| [lab03-sched-policies](lab03-sched-policies/) | SCHED_OTHER, SCHED_FIFO, SCHED_RR on one CPU, RT throttling | 1 |
| [lab04-periodic-tasks](lab04-periodic-tasks/) | Periodic tasks, deadline misses, rate-monotonic control application | 1 |
| [lab05-priority-inversion](lab05-priority-inversion/) | Unbounded priority inversion and priority inheritance | 2 |
| [lab06-deadlock](lab06-deadlock/) | Deadlock, detection by timeout, prevention by lock ordering | 3 |
| [lab07-lock-free](lab07-lock-free/) | Lock-free stack, the ABA problem, locks vs atomics under priorities | 4 |

`common/rt.h` holds the few real-time building blocks used from Lab 2 on:
memory locking, creating real-time threads, CPU pinning, absolute sleeps and
priority-inheritance mutexes. Read it once. Lab 0 shows the same steps written
out in full.

The theory behind the labs, in Romanian, is in a separate repository shared
with the ESP32-C3 lab track: [rt-concepts](https://github.com/automatica-cluj/rt-concepts).
Each lab guide lists the pages to read before the session.

[docs/](docs/) has a concepts reference, a scheduling policies guide, and
[the C you need for these labs](docs/c_for_these_labs.md), a short page for
anyone whose C is rusty. The code is plain C11 and kept simple on purpose: the
subject is real-time behaviour, and every real-time call the labs use is a C
function.

## What is on the machine

- GCC 14, `make`, `cmake`, `gdb`, `valgrind`, `strace`, `perf`
- `rt-tests` (`cyclictest`, `pi_stress`, `hackbench`), `stress-ng`, `trace-cmd`, `tuna`
- `tmux`, `htop`, `vim`, Python 3 with numpy and matplotlib (Debian packages;
  `pip install` is blocked by Debian)
- No sudo, except `sudo trace-cmd`. Real-time priorities up to 80 work without
  it (`ulimit -r`), and so does `mlockall` (`ulimit -l`).

## Working locally with Docker

The `docker/` directory builds a Debian 13 image with the same packages and
the same limits as a student account: a normal user, `rtprio 80`, 256 MiB of
lockable memory, and no `--privileged`. Use it to build and test the labs
offline:

```
make docker-build      # once
make docker-shell      # a shell in /workspace, the repository is mounted
make docker-smoke      # build every lab with -Werror and run its checks
```

A container uses the host's kernel, which is not `PREEMPT_RT`. On macOS and
Windows that kernel also runs inside a VM. Programs behave the same, but
latency numbers are much worse. Take measurements on the lab machine.

## House rules

The lab machine is one virtual machine shared by the whole class.

- Real-time tasks at priority 80 run ahead of everything else on the machine,
  including other students' shells. Don't leave them running; `pkill -u $USER <name>`.
- Load generators (`stress-ng`, `hackbench`) always get a `--timeout`.
- Programs that pin to one CPU pick a CPU from your user id so that the class
  spreads out. Keep that default unless a lab asks otherwise.
- Latency numbers measured here are for comparing your own experiments. They
  are not absolute benchmarks: twenty people share the same CPUs.
- Your home directory is private. The whole machine is wiped at the end of term.

## For maintainers

`make smoke` (inside the container or on the lab machine) builds every lab
with `-Werror` and runs each lab's `smoke` target: short runs at priority 0
and at a real-time priority, with checks on the key output lines. Run it after
every change. Sample outputs in lab READMEs marked "local Docker image" should
be replaced with output captured on the lab machine.
