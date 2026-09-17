// time_basics.cpp - reading and subtracting high-resolution timestamps
//
// The program:
//   1. reads CLOCK_MONOTONIC and prints the raw timespec
//   2. converts an interval to ns, us, ms and s
//   3. sleeps for several durations and shows how late each sleep ends
//   4. does the same with std::chrono, which is built on the same clock
//
// Usage:  ./time_basics
//
// CLOCK_MONOTONIC never jumps (NTP, date changes). CLOCK_REALTIME is wall-clock
// time and can jump backwards, so never use it to measure intervals.

#include <time.h>

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <thread>

namespace {

constexpr long kNsPerSec = 1'000'000'000L;

// A timespec is two fields: whole seconds and the nanoseconds past them.
long long to_ns(const timespec& t) {
    return static_cast<long long>(t.tv_sec) * kNsPerSec + t.tv_nsec;
}

// (a - b) in nanoseconds. tv_nsec may go negative in the subtraction; the
// seconds part makes up for it, so no special case is needed.
long long diff_ns(const timespec& a, const timespec& b) {
    return static_cast<long long>(a.tv_sec - b.tv_sec) * kNsPerSec + (a.tv_nsec - b.tv_nsec);
}

// Relative sleep with nanosecond resolution.
void sleep_ns(long ns) {
    timespec req{ns / kNsPerSec, ns % kNsPerSec};
    nanosleep(&req, nullptr);
}

}  // namespace

int main() {
    // 1. One timestamp. The absolute value is "time since boot"; only
    //    differences between two readings are meaningful.
    timespec start{};
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        std::perror("clock_gettime");
        return 1;
    }
    std::printf("1. now = %lld s + %ld ns  (= %lld ns since boot)\n",
                static_cast<long long>(start.tv_sec), start.tv_nsec, to_ns(start));

    // 2. An interval in different units.
    sleep_ns(100 * 1'000'000L);
    timespec end{};
    clock_gettime(CLOCK_MONOTONIC, &end);
    const long long elapsed = diff_ns(end, start);
    std::printf("\n2. slept 100 ms, measured:\n");
    std::printf("   %lld ns = %.1f us = %.3f ms = %.6f s\n", elapsed, elapsed / 1e3, elapsed / 1e6,
                elapsed / 1e9);

    // 3. Sleeps always end late, never early. How late depends on the
    //    scheduler, the load, the timer slack and the kernel.
    std::printf("\n3. requested vs measured sleep\n");
    std::printf("   target ms | measured ms | late by us\n");
    for (long ms : {1L, 5L, 10L, 50L}) {
        clock_gettime(CLOCK_MONOTONIC, &start);
        sleep_ns(ms * 1'000'000L);
        clock_gettime(CLOCK_MONOTONIC, &end);
        const long long measured = diff_ns(end, start);
        std::printf("   %9ld | %11.3f | %10.1f\n", ms, measured / 1e6,
                    (measured - ms * 1'000'000LL) / 1e3);
    }

    // 4. std::chrono::steady_clock is CLOCK_MONOTONIC on Linux. It is fine
    //    for measuring; the labs still use timespec when calling POSIX APIs
    //    such as clock_nanosleep, which take a timespec.
    using namespace std::chrono;
    const auto t0 = steady_clock::now();
    std::this_thread::sleep_for(milliseconds(10));
    const auto late = duration_cast<microseconds>(steady_clock::now() - t0 - milliseconds(10));
    std::printf("\n4. std::chrono: sleep_for(10ms) ended %lld us late\n",
                static_cast<long long>(late.count()));
    return 0;
}
