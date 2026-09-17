// rt.hpp - the real-time building blocks shared by all labs
//
// Lab 0 (hello_rt.cpp) spells every step out inline. From lab 1 on the same
// steps live here so each lab can focus on its own topic. Read this file once;
// it is short and every function is one idea:
//
//   lock_memory()      mlockall + no malloc trimming: no page faults later
//   set_self_sched()   switch the calling thread to SCHED_FIFO/RR/OTHER
//   start_thread()     create a thread that is real-time from its first line
//   pin_self_to_cpu()  keep the calling thread on one CPU
//   sleep_until()      absolute clock_nanosleep, the basis of periodic tasks
//   Mutex              pthread mutex with or without priority inheritance
//   thread_cpu_ns()    CPU time this thread has used
//   effective_rt_priority()  the priority the scheduler is using right now
//   Stats              min / avg / max in microseconds
//
// Why not std::thread and std::mutex? std::thread has no way to choose a
// scheduling policy before the thread starts, and std::mutex has no priority
// inheritance. Both are thin wrappers around pthreads, so we use pthreads
// directly where it matters and keep the rest of the code plain C++17.

#pragma once

#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string.h>  // strtok_r: POSIX, not in <cstring>
#include <limits>

namespace rt {

constexpr long kNsPerSec = 1'000'000'000L;
constexpr long kNsPerMs = 1'000'000L;
constexpr long kNsPerUs = 1'000L;

// ---------------------------------------------------------------- time ----

// Advance a timespec by ns (ns may be larger than one second).
inline void add_ns(timespec& t, long long ns) {
    ns += t.tv_nsec;
    t.tv_sec += static_cast<time_t>(ns / kNsPerSec);
    t.tv_nsec = static_cast<long>(ns % kNsPerSec);
}

// (a - b) in nanoseconds.
inline long long diff_ns(const timespec& a, const timespec& b) {
    return static_cast<long long>(a.tv_sec - b.tv_sec) * kNsPerSec + (a.tv_nsec - b.tv_nsec);
}

inline timespec now() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

inline long long elapsed_ns(const timespec& since) { return diff_ns(now(), since); }
inline double elapsed_ms(const timespec& since) { return elapsed_ns(since) / 1e6; }

// CPU time used by the calling thread, as opposed to wall-clock time. Work
// measured in CPU time really does take longer when the thread is preempted.
inline long long thread_cpu_ns() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<long long>(t.tv_sec) * kNsPerSec + t.tv_nsec;
}

// Sleep until an absolute CLOCK_MONOTONIC time. The deadline does not move if
// we are woken early by a signal, so we simply sleep again.
// Note: clock_nanosleep returns the error number, it does not set errno.
inline void sleep_until(const timespec& deadline) {
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
    } while (rc == EINTR);
}

inline void sleep_ms(long ms) {
    timespec t = now();
    add_ns(t, static_cast<long long>(ms) * kNsPerMs);
    sleep_until(t);
}

// Burn CPU for about ns nanoseconds. Stands in for "real computation".
inline void busy_ns(long long ns) {
    const timespec start = now();
    volatile unsigned long x = 0;
    while (elapsed_ns(start) < ns) {
        for (int i = 0; i < 1000; ++i) x += i;
    }
}

// ---------------------------------------------------------- scheduling ----

inline const char* policy_name(int policy) {
    switch (policy) {
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_RR: return "SCHED_RR";
        case SCHED_OTHER: return "SCHED_OTHER";
        default: return "other";
    }
}

// Print why a scheduling call failed, with a hint for the usual cause.
inline void explain_sched_error(const char* what, int rc, int prio) {
    std::fprintf(stderr, "%s(priority %d): %s\n", what, prio, std::strerror(rc));
    if (rc == EPERM) {
        std::fprintf(stderr, "hint: 'ulimit -r' shows the highest real-time priority you may use\n");
    }
}

// priority 0 means SCHED_OTHER, anything else uses `policy` (FIFO by default).
// Returns 0 or the error number.
inline int set_self_sched(int priority, int policy = SCHED_FIFO) {
    sched_param sp{};
    if (priority == 0) policy = SCHED_OTHER;
    sp.sched_priority = priority;
    int rc = pthread_setschedparam(pthread_self(), policy, &sp);
    if (rc != 0) explain_sched_error("pthread_setschedparam", rc, priority);
    return rc;
}

// Read back what the kernel actually gave us. Never trust what you asked for.
inline void print_self_sched(const char* who) {
    int policy = 0;
    sched_param sp{};
    pthread_getschedparam(pthread_self(), &policy, &sp);
    std::printf("%s: %s priority %d, CPU %d\n", who, policy_name(policy), sp.sched_priority,
                sched_getcpu());
}

// The priority the scheduler is using for this thread right now, including a
// boost from priority inheritance. pthread_getschedparam() reports only what
// we asked for ourselves, so read field 18 of /proc/thread-self/stat: for a
// real-time thread it holds -1 - effective_priority.
inline int effective_rt_priority() {
    FILE* f = std::fopen("/proc/thread-self/stat", "r");
    if (f == nullptr) return -1;
    char line[512];
    char* got = std::fgets(line, sizeof(line), f);
    std::fclose(f);
    if (got == nullptr) return -1;

    // Field 2 is the thread name in brackets and may contain spaces, so start
    // after the last ')'. The next token is field 3.
    char* rest = std::strrchr(line, ')');
    if (rest == nullptr) return -1;
    ++rest;

    char* save = nullptr;  // strtok_r keeps its state here, so this is safe
    char* token = strtok_r(rest, " ", &save);  // to call from any thread
    for (int field = 3; token != nullptr; ++field) {
        if (field == 18) return -std::atoi(token) - 1;
        token = strtok_r(nullptr, " ", &save);
    }
    return -1;
}

inline int self_priority() {
    int policy = 0;
    sched_param sp{};
    pthread_getschedparam(pthread_self(), &policy, &sp);
    return sp.sched_priority;
}

// ---------------------------------------------------------------- CPUs ----

inline int pin_self_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) std::fprintf(stderr, "pin to CPU %d: %s\n", cpu, std::strerror(rc));
    return rc;
}

// A CPU for single-CPU experiments. On the shared machine twenty students
// pinning to CPU 0 would measure each other, so the default is derived from
// the user id: different accounts land on different CPUs.
inline int default_cpu() {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return 0;
    const int count = CPU_COUNT(&set);
    if (count <= 0) return 0;
    int wanted = static_cast<int>(getuid() % static_cast<unsigned>(count));
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &set) && wanted-- == 0) return cpu;
    }
    return 0;
}

// Create a thread whose policy, priority and CPU are set *before* it runs.
//   priority 0 -> SCHED_OTHER, cpu < 0 -> no pinning.
// PTHREAD_EXPLICIT_SCHED is essential: without it the attributes are ignored
// and the new thread silently inherits the creator's policy.
// Returns 0 or the error number (EPERM when the priority is over the limit).
inline int start_thread(pthread_t* thread, void* (*fn)(void*), void* arg, int priority,
                        int cpu = -1, int policy = SCHED_FIFO) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (priority == 0) policy = SCHED_OTHER;
    sched_param sp{};
    sp.sched_priority = priority;
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, policy);
    pthread_attr_setschedparam(&attr, &sp);
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
    }
    int rc = pthread_create(thread, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) explain_sched_error("pthread_create", rc, priority);
    return rc;
}

// -------------------------------------------------------------- memory ----

// Lock all memory in RAM and stop malloc from giving memory back to the
// kernel. After this, the time-critical code cannot take a page fault.
inline bool lock_memory() {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::perror("mlockall");  // mlockall does set errno
        std::fprintf(stderr, "hint: check 'ulimit -l' (memlock limit)\n");
        return false;
    }
    mallopt(M_TRIM_THRESHOLD, -1);  // never return freed heap memory
    mallopt(M_MMAP_MAX, 0);         // serve large allocations from the heap
    // Touch a chunk of stack now so its pages are already present.
    volatile char stack[64 * 1024];
    for (size_t i = 0; i < sizeof(stack); i += 4096) stack[i] = 0;
    return true;
}

// --------------------------------------------------------------- mutex ----

// A pthread mutex that works with std::lock_guard / std::unique_lock.
//   Mutex m(Mutex::kInherit)  priority inheritance (PTHREAD_PRIO_INHERIT)
//   Mutex m(Mutex::kNone)     plain mutex, same behaviour as std::mutex
class Mutex {
public:
    enum Protocol { kNone = PTHREAD_PRIO_NONE, kInherit = PTHREAD_PRIO_INHERIT };

    explicit Mutex(Protocol protocol = kNone) {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        int rc = pthread_mutexattr_setprotocol(&attr, protocol);
        if (rc != 0) {
            std::fprintf(stderr, "pthread_mutexattr_setprotocol: %s\n", std::strerror(rc));
            std::exit(1);
        }
        pthread_mutex_init(&m_, &attr);
        pthread_mutexattr_destroy(&attr);
    }
    ~Mutex() { pthread_mutex_destroy(&m_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() { pthread_mutex_lock(&m_); }
    void unlock() { pthread_mutex_unlock(&m_); }
    pthread_mutex_t* native() { return &m_; }

private:
    pthread_mutex_t m_;
};

// --------------------------------------------------------------- stats ----

struct Stats {
    long long min = std::numeric_limits<long long>::max();
    long long max = std::numeric_limits<long long>::min();
    long long sum = 0;
    long long n = 0;

    void add(long long ns) {
        min = std::min(min, ns);
        max = std::max(max, ns);
        sum += ns;
        ++n;
    }
    long long avg() const { return n ? sum / n : 0; }

    // Real-time is about the worst case: read the max, not the avg.
    void print_us(const char* label) const {
        if (n == 0) {
            std::printf("%s: no samples\n", label);
            return;
        }
        std::printf("%s over %lld samples: min %lld us, avg %lld us, max %lld us\n", label, n,
                    min / kNsPerUs, avg() / kNsPerUs, max / kNsPerUs);
    }
};

// ----------------------------------------------------------- arguments ----

// Positional integer argument argv[index] with a default and a valid range.
// Exits with a message on garbage such as "abc" or out-of-range values.
inline long arg_long(int argc, char** argv, int index, long fallback, long lo, long hi) {
    if (argc <= index) return fallback;
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(argv[index], &end, 10);
    if (errno != 0 || end == argv[index] || *end != '\0' || v < lo || v > hi) {
        std::fprintf(stderr, "argument %d ('%s') must be an integer in [%ld, %ld]\n", index,
                     argv[index], lo, hi);
        std::exit(2);
    }
    return v;
}

}  // namespace rt
