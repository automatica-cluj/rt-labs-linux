// latency.cpp - how late does a periodic task wake up?
//
// The program:
//   1. locks its memory and optionally switches to SCHED_FIFO
//   2. wakes up every period for a number of seconds, using either
//        abs: clock_nanosleep(TIMER_ABSTIME) until the next deadline, or
//        rel: a relative sleep of one period after the work is done
//   3. stores how late each wake-up was (no printing inside the loop)
//   4. prints min / avg / max, a histogram, and how far the schedule drifted
//   5. optionally writes every sample to a CSV file for plot_latency.py
//
// Usage:  ./latency [priority] [seconds] [mode] [period-us] [csv-file]
//   ./latency                    SCHED_FIFO 80, 5 s, abs, 1000 us
//   ./latency 0 5                SCHED_OTHER, for comparison
//   ./latency 80 5 rel           relative sleep: watch the drift line
//   ./latency 80 10 abs 1000 fifo.csv
//
// Latency = actual wake-up time - intended wake-up time. It is never negative.
// Jitter  = how much the latency varies (max - min).

#include "rt.hpp"

#include <cstring>
#include <fstream>
#include <vector>

namespace {

void print_histogram(const std::vector<long long>& samples) {
    // Bucket upper bounds in microseconds; the last bucket is open-ended.
    const long long bounds_us[] = {10, 20, 50, 100, 200, 500, 1000, 5000};
    const int kBuckets = sizeof(bounds_us) / sizeof(bounds_us[0]) + 1;
    long long counts[kBuckets] = {};
    for (long long ns : samples) {
        int b = 0;
        while (b < kBuckets - 1 && ns >= bounds_us[b] * rt::kNsPerUs) ++b;
        ++counts[b];
    }
    std::printf("histogram:\n");
    long long lower = 0;
    for (int b = 0; b < kBuckets; ++b) {
        if (b < kBuckets - 1) {
            std::printf("  %5lld .. %5lld us  %8lld\n", lower, bounds_us[b], counts[b]);
            lower = bounds_us[b];
        } else {
            std::printf("  %5lld us and more %8lld\n", lower, counts[b]);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    const int priority = static_cast<int>(rt::arg_long(argc, argv, 1, 80, 0, 99));
    const long seconds = rt::arg_long(argc, argv, 2, 5, 1, 3600);
    const char* mode = argc > 3 ? argv[3] : "abs";
    const long period_us = rt::arg_long(argc, argv, 4, 1000, 100, 1'000'000);
    const char* csv = argc > 5 ? argv[5] : nullptr;

    const bool absolute = std::strcmp(mode, "abs") == 0;
    if (!absolute && std::strcmp(mode, "rel") != 0) {
        std::fprintf(stderr, "mode must be 'abs' or 'rel'\n");
        return 2;
    }
    const long long period_ns = period_us * rt::kNsPerUs;
    const long iterations = seconds * 1'000'000L / period_us;

    // Allocate all storage before locking memory and before the loop.
    std::vector<long long> samples;
    samples.reserve(iterations);

    if (!rt::lock_memory()) return 1;
    if (rt::set_self_sched(priority) != 0) return 1;
    rt::print_self_sched("latency");
    std::printf("mode %s, period %ld us, %ld iterations\n", mode, period_us, iterations);

    const timespec start = rt::now();
    timespec next = start;
    rt::add_ns(next, period_ns);

    for (long i = 0; i < iterations; ++i) {
        if (absolute) {
            // Sleep until a fixed point in time. The next deadline is computed
            // from the previous deadline, so errors never accumulate.
            rt::sleep_until(next);
        } else {
            // Sleep "one period from now". Every late wake-up pushes all later
            // wake-ups back: the schedule drifts.
            timespec rel{0, static_cast<long>(period_ns)};
            clock_nanosleep(CLOCK_MONOTONIC, 0, &rel, nullptr);
        }
        const timespec woke = rt::now();
        // The intended wake-up time: next for abs; one period after the
        // previous wake-up for rel (that is what the program asked for).
        samples.push_back(rt::diff_ns(woke, next));
        if (absolute) {
            rt::add_ns(next, period_ns);
        } else {
            next = woke;
            rt::add_ns(next, period_ns);
        }
    }

    // Everything below runs after the time-critical part.
    const long long total_ns = rt::elapsed_ns(start);
    rt::Stats stats;
    for (long long s : samples) stats.add(s);
    stats.print_us("wake-up latency");
    std::printf("jitter (max - min): %lld us\n", (stats.max - stats.min) / rt::kNsPerUs);
    print_histogram(samples);
    std::printf("drift: %ld periods should take %.3f ms, took %.3f ms (%+.3f ms)\n", iterations,
                iterations * period_ns / 1e6, total_ns / 1e6,
                (total_ns - iterations * period_ns) / 1e6);

    if (csv) {
        std::ofstream out(csv);
        out << "# iteration latency_us  (" << rt::policy_name(priority ? SCHED_FIFO : SCHED_OTHER)
            << " priority " << priority << ", mode " << mode << ", period " << period_us
            << " us)\n";
        for (std::size_t i = 0; i < samples.size(); ++i) {
            out << i << ' ' << samples[i] / 1e3 << '\n';
        }
        if (!out) {
            std::perror(csv);
            return 1;
        }
        std::printf("samples written to %s\n", csv);
    }
    return 0;
}
