/*
 * statistics.c - the numbers that describe latency measurements
 *
 * The program generates 1000 simulated latencies (mostly 50..250 us, with
 * rare large spikes) and computes:
 *   min, max, mean, jitter (max - min), standard deviation,
 *   percentiles (50, 90, 99, 99.9), and a histogram.
 *
 * Usage:  ./statistics [seed]
 *
 * For real-time work the tail is what matters. A task with a mean of 100 us
 * and a max of 10 ms misses deadlines, however good the mean looks.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SAMPLES 1000

/* Comparison function for qsort: negative, zero or positive, like strcmp. */
static int compare_ll(const void *a, const void *b) {
    long long x = *(const long long *)a;
    long long y = *(const long long *)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/*
 * p-th percentile of sorted data, nearest-rank method: the smallest value
 * that at least p % of the samples are less than or equal to.
 */
static long long percentile(const long long sorted[], int count, double p) {
    int rank = (int)ceil(p / 100.0 * count);
    if (rank < 1) rank = 1;
    return sorted[rank - 1];
}

int main(int argc, char **argv) {
    unsigned seed = 1;
    if (argc > 1) seed = (unsigned)atoi(argv[1]);

    // Simulated latencies in nanoseconds: 50..250 us, plus a rare spike that
    // stands for a preemption or a page fault. The same seed gives the same
    // data every run, so your numbers are repeatable.
    srand(seed);
    long long samples[SAMPLES];
    for (int i = 0; i < SAMPLES; ++i) {
        samples[i] = 50000 + rand() % 200001;
        if (rand() % 1000 < 5) samples[i] *= 20;  /* 0.5 % of the samples */
    }

    // min, max and mean in one pass.
    long long min = samples[0];
    long long max = samples[0];
    double sum = 0;
    for (int i = 0; i < SAMPLES; ++i) {
        if (samples[i] < min) min = samples[i];
        if (samples[i] > max) max = samples[i];
        sum += samples[i];
    }
    const double mean = sum / SAMPLES;

    // Standard deviation: the average distance from the mean. It needs the
    // mean first, so this is a second pass.
    double squares = 0;
    for (int i = 0; i < SAMPLES; ++i) {
        squares += (samples[i] - mean) * (samples[i] - mean);
    }
    const double stddev = sqrt(squares / SAMPLES);

    printf("%d samples (seed %u)\n", SAMPLES, seed);
    printf("  min     %8.1f us\n", min / 1e3);
    printf("  mean    %8.1f us\n", mean / 1e3);
    printf("  max     %8.1f us\n", max / 1e3);
    printf("  jitter  %8.1f us  (max - min)\n", (max - min) / 1e3);
    printf("  stddev  %8.1f us\n", stddev / 1e3);

    // Percentiles need sorted data. Sort a copy so the original order stays.
    long long sorted[SAMPLES];
    for (int i = 0; i < SAMPLES; ++i) sorted[i] = samples[i];
    qsort(sorted, SAMPLES, sizeof(sorted[0]), compare_ll);

    const double wanted[] = {50.0, 90.0, 99.0, 99.9};
    printf("\npercentiles\n");
    for (int i = 0; i < 4; ++i) {
        printf("  p%-5g %8.1f us\n", wanted[i], percentile(sorted, SAMPLES, wanted[i]) / 1e3);
    }

    // Histogram. The buckets are half-open, [from, to), so every sample is
    // counted exactly once and the percentages add up to 100.
    const long long bucket_from[] = {0, 100000, 200000, 300000, 1000000};
    const long long bucket_to[] = {100000, 200000, 300000, 1000000, 1000000000};
    const char *bucket_label[] = {"  0 .. 100 us", "100 .. 200 us", "200 .. 300 us",
                                  "300 us .. 1 ms", "     >= 1 ms"};

    printf("\nhistogram\n");
    for (int b = 0; b < 5; ++b) {
        int count = 0;
        for (int i = 0; i < SAMPLES; ++i) {
            if (samples[i] >= bucket_from[b] && samples[i] < bucket_to[b]) count++;
        }
        printf("  %-14s %5d  %5.1f %%  ", bucket_label[b], count, 100.0 * count / SAMPLES);
        for (int i = 0; i < count / 10; ++i) putchar('#');
        putchar('\n');
    }
    return 0;
}
