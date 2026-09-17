// inversion.cpp - priority inversion, and priority inheritance as the fix
//
// Three SCHED_FIFO threads share one CPU and one mutex:
//
//   LOW    priority 20  locks the mutex, then needs CS ms of CPU inside it
//   HIGH   priority 60  arrives while LOW holds the mutex and blocks on it
//   MEDIUM priority 40  does not touch the mutex, just burns MEDIUM ms of CPU
//
// With a plain mutex MEDIUM preempts LOW, so HIGH waits for MEDIUM too: the
// blocking time grows with MEDIUM's work and has no bound (unbounded
// inversion). With PTHREAD_PRIO_INHERIT the kernel lends HIGH's priority to
// LOW while HIGH waits, MEDIUM cannot preempt LOW, and HIGH waits at most for
// the rest of LOW's critical section (bounded inversion).
//
// Usage:  ./inversion [none|inherit] [cpu] [medium_ms] [cs_ms]
//   none       plain mutex (PTHREAD_PRIO_NONE)            default
//   inherit    priority inheritance (PTHREAD_PRIO_INHERIT)
//   cpu        CPU all three threads share; -1 = no pinning  (default: per user)
//   medium_ms  CPU time MEDIUM burns                      (default 200)
//   cs_ms      CPU time LOW needs inside the mutex        (default 50)
//
// The sequence is driven by semaphores, not by sleeps, so it is the same on
// every run: LOW locks -> HIGH requests -> MEDIUM starts.

#include "rt.hpp"

#include <semaphore.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kPrioLow = 20;
constexpr int kPrioMedium = 40;
constexpr int kPrioHigh = 60;
constexpr long kHighWorkMs = 5;

struct Scenario {
    rt::Mutex* mutex = nullptr;
    long medium_ns = 0;
    long cs_ns = 0;

    sem_t low_locked;       // LOW holds the mutex
    sem_t high_requesting;  // HIGH is about to call lock()

    // Timestamps. Each is written by exactly one thread and read by main
    // after all threads have been joined.
    timespec t0{};
    timespec low_lock{}, low_unlock{};
    timespec high_request{}, high_acquired{};
    timespec medium_start{}, medium_end{};
    int low_cpu = -1, medium_cpu = -1, high_cpu = -1;

    // LOW samples its effective priority once HIGH is waiting.
    std::atomic<bool> high_waiting{false};
    int low_effective_prio = -1;
};

// Work is measured in CPU time (rt::thread_cpu_ns), not wall-clock time: if
// LOW is preempted, its critical section really does take longer. The
// priority LOW is running at, boost included, comes from
// rt::effective_rt_priority(). Both live in ../common/rt.hpp.

void* low_task(void* arg) {
    auto* s = static_cast<Scenario*>(arg);
    s->low_cpu = sched_getcpu();

    s->mutex->lock();
    s->low_lock = rt::now();
    sem_post(&s->low_locked);

    // Critical section: needs cs_ns of CPU, however long that takes.
    const long long start = rt::thread_cpu_ns();
    bool sampled = false;
    volatile unsigned long x = 0;
    while (rt::thread_cpu_ns() - start < s->cs_ns) {
        for (int i = 0; i < 1000; ++i) x += i;
        if (!sampled && s->high_waiting.load()) {
            s->low_effective_prio = rt::effective_rt_priority();
            sampled = true;
        }
    }

    s->low_unlock = rt::now();
    s->mutex->unlock();
    return nullptr;
}

void* high_task(void* arg) {
    auto* s = static_cast<Scenario*>(arg);
    s->high_cpu = sched_getcpu();

    s->high_request = rt::now();
    sem_post(&s->high_requesting);
    s->high_waiting.store(true);

    s->mutex->lock();
    s->high_acquired = rt::now();
    rt::busy_ns(kHighWorkMs * rt::kNsPerMs);  // short use of the resource
    s->mutex->unlock();
    return nullptr;
}

void* medium_task(void* arg) {
    auto* s = static_cast<Scenario*>(arg);
    s->medium_cpu = sched_getcpu();
    s->medium_start = rt::now();
    const long long start = rt::thread_cpu_ns();
    volatile unsigned long x = 0;
    while (rt::thread_cpu_ns() - start < s->medium_ns) {
        for (int i = 0; i < 1000; ++i) x += i;
    }
    s->medium_end = rt::now();
    return nullptr;
}

double ms(const timespec& t, const timespec& t0) { return rt::diff_ns(t, t0) / 1e6; }

// Length of the overlap of [a0, a1] and [b0, b1] in ms.
double overlap_ms(const timespec& a0, const timespec& a1, const timespec& b0,
                  const timespec& b1) {
    const timespec& start = rt::diff_ns(a0, b0) > 0 ? a0 : b0;
    const timespec& end = rt::diff_ns(a1, b1) < 0 ? a1 : b1;
    const long long d = rt::diff_ns(end, start);
    return d > 0 ? d / 1e6 : 0.0;
}

void start_or_die(pthread_t* t, void* (*fn)(void*), Scenario* s, int prio, int cpu,
                  const char* name) {
    if (rt::start_thread(t, fn, s, prio, cpu) != 0) {
        std::fprintf(stderr, "could not start %s as SCHED_FIFO %d; this lab needs real-time "
                             "priorities (see lab00 check_env.sh)\n", name, prio);
        std::exit(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    const char* protocol = argc > 1 ? argv[1] : "none";
    const bool inherit = std::strcmp(protocol, "inherit") == 0;
    if (!inherit && std::strcmp(protocol, "none") != 0) {
        std::fprintf(stderr, "usage: %s [none|inherit] [cpu] [medium_ms] [cs_ms]\n", argv[0]);
        return 2;
    }
    const int cpu = static_cast<int>(rt::arg_long(argc, argv, 2, rt::default_cpu(), -1, CPU_SETSIZE - 1));
    const long medium_ms = rt::arg_long(argc, argv, 3, 200, 0, 500);
    const long cs_ms = rt::arg_long(argc, argv, 4, 50, 1, 300);

    if (!rt::lock_memory()) return 1;

    rt::Mutex mutex(inherit ? rt::Mutex::kInherit : rt::Mutex::kNone);
    Scenario s;
    s.mutex = &mutex;
    s.medium_ns = medium_ms * rt::kNsPerMs;
    s.cs_ns = cs_ms * rt::kNsPerMs;
    sem_init(&s.low_locked, 0, 0);
    sem_init(&s.high_requesting, 0, 0);

    std::printf("mutex protocol: %s\n",
                inherit ? "PTHREAD_PRIO_INHERIT" : "PTHREAD_PRIO_NONE");
    if (cpu >= 0) {
        std::printf("all threads pinned to CPU %d\n", cpu);
    } else {
        std::printf("no pinning: threads may run on different CPUs\n");
    }
    std::printf("LOW prio %d needs %ld ms inside the mutex, MEDIUM prio %d burns %ld ms, "
                "HIGH prio %d\n\n", kPrioLow, cs_ms, kPrioMedium, medium_ms, kPrioHigh);

    // main stays SCHED_OTHER and only orchestrates. It is not pinned, so it
    // runs on another CPU and does not disturb the three real-time threads.
    pthread_t low, medium, high;
    s.t0 = rt::now();
    start_or_die(&low, low_task, &s, kPrioLow, cpu, "LOW");
    sem_wait(&s.low_locked);
    start_or_die(&high, high_task, &s, kPrioHigh, cpu, "HIGH");
    sem_wait(&s.high_requesting);
    rt::sleep_ms(1);  // let HIGH actually go to sleep on the mutex
    start_or_die(&medium, medium_task, &s, kPrioMedium, cpu, "MEDIUM");

    pthread_join(high, nullptr);
    pthread_join(medium, nullptr);
    pthread_join(low, nullptr);
    sem_destroy(&s.low_locked);
    sem_destroy(&s.high_requesting);

    // Timeline, printed only now: printf inside the threads would change the
    // very timing we are trying to observe.
    struct Event {
        timespec t;
        const char* what;
        int cpu;
    };
    const int kEvents = 6;
    Event events[kEvents] = {
        {s.low_lock, "LOW    locks the mutex", s.low_cpu},
        {s.high_request, "HIGH   requests the mutex and blocks", s.high_cpu},
        {s.medium_start, "MEDIUM starts running", s.medium_cpu},
        {s.medium_end, "MEDIUM finishes", -1},
        {s.low_unlock, "LOW    unlocks", -1},
        {s.high_acquired, "HIGH   gets the mutex", -1},
    };
    // Put the events in time order. Six entries, so a plain bubble sort.
    for (int i = 0; i + 1 < kEvents; ++i) {
        for (int j = 0; j + 1 < kEvents - i; ++j) {
            if (rt::diff_ns(events[j + 1].t, events[j].t) < 0) {
                const Event tmp = events[j];
                events[j] = events[j + 1];
                events[j + 1] = tmp;
            }
        }
    }

    std::printf("timeline (ms since start)\n");
    for (int i = 0; i < kEvents; ++i) {
        if (events[i].cpu >= 0) {
            std::printf("  %7.1f  %-38s (CPU %d)\n", ms(events[i].t, s.t0), events[i].what,
                        events[i].cpu);
        } else {
            std::printf("  %7.1f  %s\n", ms(events[i].t, s.t0), events[i].what);
        }
    }
    if (s.low_effective_prio >= 0) {
        std::printf("\nLOW's effective priority while HIGH waited: %d\n", s.low_effective_prio);
    }

    const double blocked = ms(s.high_acquired, s.high_request);
    const double medium_inside = overlap_ms(s.medium_start, s.medium_end, s.high_request, s.high_acquired);
    std::printf("\nHIGH blocked for %.1f ms (LOW's whole critical section is %ld ms)\n", blocked, cs_ms);
    std::printf("MEDIUM ran for %.1f ms of that time\n", medium_inside);

    // The bound with priority inheritance is the critical section itself.
    // Allow a little slack for thread start-up and timer resolution.
    const double bound = cs_ms * 1.2 + 5.0;
    if (blocked > bound) {
        std::printf("UNBOUNDED: HIGH waited %.1f ms, longer than the %ld ms critical section, "
                    "because MEDIUM ran in between\n", blocked, cs_ms);
    } else {
        std::printf("BOUNDED: HIGH waited %.1f ms, no longer than the %ld ms critical section\n",
                    blocked, cs_ms);
    }
    return 0;
}
