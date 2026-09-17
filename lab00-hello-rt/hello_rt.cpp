// hello_rt.cpp - first contact with a PREEMPT_RT kernel
//
// The program:
//   1. locks its memory so no page fault can happen mid-loop
//   2. moves itself to SCHED_FIFO at the priority given on the command line
//   3. wakes up every 1 ms for a few seconds using an absolute timer
//   4. reports how late each wake-up was (min / avg / max, in microseconds)
//
// Usage:  ./hello_rt [priority] [seconds]
//   priority 0  -> stay a normal (SCHED_OTHER) task, for comparison
//   priority 1..99 -> SCHED_FIFO at that priority (students are capped at 80)
//
// Everything here is plain POSIX and glibc. There is no "real-time C++".

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr long kNsPerSec = 1'000'000'000L;
constexpr long kPeriodNs = 1'000'000L;  // 1 ms

// Advance a timespec by ns, keeping tv_nsec in range.
void add_ns(timespec& t, long ns) {
    t.tv_nsec += ns;
    while (t.tv_nsec >= kNsPerSec) {
        t.tv_nsec -= kNsPerSec;
        t.tv_sec += 1;
    }
}

// (a - b) in nanoseconds.
long diff_ns(const timespec& a, const timespec& b) {
    return (a.tv_sec - b.tv_sec) * kNsPerSec + (a.tv_nsec - b.tv_nsec);
}

const char* policy_name(int policy) {
    switch (policy) {
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_RR: return "SCHED_RR";
        case SCHED_OTHER: return "SCHED_OTHER";
        default: return "other";
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int priority = argc > 1 ? std::atoi(argv[1]) : 80;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
    const long iterations = seconds * (kNsPerSec / kPeriodNs);

    // 1. Lock all current and future pages in RAM. A page fault inside the
    //    loop would cost far more than the period we are trying to hold.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::perror("mlockall");
        std::fprintf(stderr, "hint: check 'ulimit -l' (memlock limit)\n");
        return 1;
    }

    // 2. Ask the kernel for a real-time policy. Non-root processes need
    //    RLIMIT_RTPRIO >= priority for this to succeed (see 'ulimit -r').
    if (priority > 0) {
        sched_param sp{};
        sp.sched_priority = priority;
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != 0) {
            std::fprintf(stderr, "pthread_setschedparam(SCHED_FIFO, %d): %s\n",
                         priority, std::strerror(rc));
            if (rc == EPERM) {
                std::fprintf(stderr, "hint: 'ulimit -r' shows the highest priority you may use\n");
            }
            return 1;
        }
    }

    int policy = 0;
    sched_param current{};
    pthread_getschedparam(pthread_self(), &policy, &current);
    std::printf("running as %s priority %d, period %ld us, %d s\n",
                policy_name(policy), current.sched_priority, kPeriodNs / 1000, seconds);

    // 3. Periodic loop with an absolute deadline. Using TIMER_ABSTIME means
    //    the schedule never drifts: each wake-up is computed from the previous
    //    deadline, not from "now".
    timespec next{};
    clock_gettime(CLOCK_MONOTONIC, &next);
    add_ns(next, kPeriodNs);

    long min_ns = kNsPerSec, max_ns = 0, sum_ns = 0;

    for (long i = 0; i < iterations; ++i) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);

        // How late did we wake up compared to the deadline we asked for?
        const long late = diff_ns(now, next);
        if (late < min_ns) min_ns = late;
        if (late > max_ns) max_ns = late;
        sum_ns += late;

        add_ns(next, kPeriodNs);
    }

    // 4. Report. Real-time is about the worst case: read the max, not the avg.
    std::printf("wake-up latency over %ld iterations:\n", iterations);
    std::printf("  min %6ld us\n", min_ns / 1000);
    std::printf("  avg %6ld us\n", sum_ns / iterations / 1000);
    std::printf("  max %6ld us\n", max_ns / 1000);
    return 0;
}
