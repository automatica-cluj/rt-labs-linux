// statistics.cpp - the numbers that describe latency measurements
//
// The program generates 1000 simulated latencies (mostly 50..250 us, with
// rare large spikes) and computes:
//   min, max, mean, jitter (max - min), standard deviation,
//   percentiles (50, 90, 99, 99.9), and a histogram.
//
// Usage:  ./statistics [seed]
//
// For real-time work the tail is what matters. A task with a mean of 100 us
// and a max of 10 ms misses deadlines, however good the mean looks.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

namespace {

// p-th percentile of sorted data, nearest-rank method.
long long percentile(const std::vector<long long>& sorted, double p) {
    std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * sorted.size()));
    if (rank == 0) rank = 1;
    return sorted[rank - 1];
}

}  // namespace

int main(int argc, char** argv) {
    const unsigned seed = argc > 1 ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 1;
    constexpr int kSamples = 1000;

    // Simulated latencies in ns. A fixed seed makes runs repeatable.
    std::mt19937 rng(seed);
    std::uniform_int_distribution<long long> base(50'000, 250'000);
    std::uniform_int_distribution<int> percent(0, 999);
    std::vector<long long> samples(kSamples);
    for (auto& s : samples) {
        s = base(rng);
        if (percent(rng) < 5) s *= 20;  // 0.5 % spikes: a preemption, a page fault...
    }

    const auto [lo, hi] = std::minmax_element(samples.begin(), samples.end());
    const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) / kSamples;
    double var = 0;
    for (long long s : samples) var += (s - mean) * (s - mean);
    const double stddev = std::sqrt(var / kSamples);

    std::printf("%d samples (seed %u)\n", kSamples, seed);
    std::printf("  min     %8.1f us\n", *lo / 1e3);
    std::printf("  mean    %8.1f us\n", mean / 1e3);
    std::printf("  max     %8.1f us\n", *hi / 1e3);
    std::printf("  jitter  %8.1f us  (max - min)\n", (*hi - *lo) / 1e3);
    std::printf("  stddev  %8.1f us\n", stddev / 1e3);

    // Percentiles need sorted data. Sort a copy; keep the original order.
    std::vector<long long> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    std::printf("\npercentiles\n");
    for (double p : {50.0, 90.0, 99.0, 99.9}) {
        std::printf("  p%-5g %8.1f us\n", p, percentile(sorted, p) / 1e3);
    }

    // Histogram with half-open buckets [from, to): every sample is counted
    // exactly once, so the percentages add up to 100.
    struct Bucket { long long from, to; const char* label; };
    const Bucket buckets[] = {
        {0, 100'000, "  0 .. 100 us"},
        {100'000, 200'000, "100 .. 200 us"},
        {200'000, 300'000, "200 .. 300 us"},
        {300'000, 1'000'000, "300 us .. 1 ms"},
        {1'000'000, INT64_MAX, "     >= 1 ms"},
    };
    std::printf("\nhistogram\n");
    for (const Bucket& b : buckets) {
        const auto n = std::count_if(samples.begin(), samples.end(),
                                     [&](long long s) { return s >= b.from && s < b.to; });
        std::printf("  %-14s %5ld  %5.1f %%  ", b.label, static_cast<long>(n), 100.0 * n / kSamples);
        for (long i = 0; i < n / 10; ++i) std::putchar('#');
        std::putchar('\n');
    }
    return 0;
}
