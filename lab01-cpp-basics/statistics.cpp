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

#include <algorithm>  // std::sort
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {

const int kSamples = 1000;

// p-th percentile of sorted data, nearest-rank method: the smallest value
// that at least p % of the samples are less than or equal to.
long long percentile(const long long sorted[], int count, double p) {
    int rank = static_cast<int>(std::ceil(p / 100.0 * count));
    if (rank < 1) rank = 1;
    return sorted[rank - 1];
}

}  // namespace

int main(int argc, char** argv) {
    const unsigned seed = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 1;

    // Simulated latencies in nanoseconds: 50..250 us, plus a rare spike that
    // stands for a preemption or a page fault. The same seed gives the same
    // data every run, so your numbers are repeatable.
    std::srand(seed);
    long long samples[kSamples];
    for (int i = 0; i < kSamples; ++i) {
        samples[i] = 50000 + std::rand() % 200001;
        if (std::rand() % 1000 < 5) samples[i] *= 20;  // 0.5 % of the samples
    }

    // min, max and mean in one pass.
    long long min = samples[0];
    long long max = samples[0];
    double sum = 0;
    for (int i = 0; i < kSamples; ++i) {
        if (samples[i] < min) min = samples[i];
        if (samples[i] > max) max = samples[i];
        sum += samples[i];
    }
    const double mean = sum / kSamples;

    // Standard deviation: the average distance from the mean. It needs the
    // mean first, so this is a second pass.
    double squares = 0;
    for (int i = 0; i < kSamples; ++i) {
        squares += (samples[i] - mean) * (samples[i] - mean);
    }
    const double stddev = std::sqrt(squares / kSamples);

    std::printf("%d samples (seed %u)\n", kSamples, seed);
    std::printf("  min     %8.1f us\n", min / 1e3);
    std::printf("  mean    %8.1f us\n", mean / 1e3);
    std::printf("  max     %8.1f us\n", max / 1e3);
    std::printf("  jitter  %8.1f us  (max - min)\n", (max - min) / 1e3);
    std::printf("  stddev  %8.1f us\n", stddev / 1e3);

    // Percentiles need sorted data. Sort a copy so the original order stays.
    long long sorted[kSamples];
    for (int i = 0; i < kSamples; ++i) sorted[i] = samples[i];
    std::sort(sorted, sorted + kSamples);

    const double wanted[] = {50.0, 90.0, 99.0, 99.9};
    std::printf("\npercentiles\n");
    for (int i = 0; i < 4; ++i) {
        std::printf("  p%-5g %8.1f us\n", wanted[i], percentile(sorted, kSamples, wanted[i]) / 1e3);
    }

    // Histogram. The buckets are half-open, [from, to), so every sample is
    // counted exactly once and the percentages add up to 100.
    const long long bucket_from[] = {0, 100000, 200000, 300000, 1000000};
    const long long bucket_to[] = {100000, 200000, 300000, 1000000, 1000000000};
    const char* bucket_label[] = {"  0 .. 100 us", "100 .. 200 us", "200 .. 300 us",
                                  "300 us .. 1 ms", "     >= 1 ms"};

    std::printf("\nhistogram\n");
    for (int b = 0; b < 5; ++b) {
        int count = 0;
        for (int i = 0; i < kSamples; ++i) {
            if (samples[i] >= bucket_from[b] && samples[i] < bucket_to[b]) count++;
        }
        std::printf("  %-14s %5d  %5.1f %%  ", bucket_label[b], count, 100.0 * count / kSamples);
        for (int i = 0; i < count / 10; ++i) std::putchar('#');
        std::putchar('\n');
    }
    return 0;
}
