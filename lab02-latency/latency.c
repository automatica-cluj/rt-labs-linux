/*
 * latency.c - how late does a periodic task wake up?
 *
 * The program:
 *   1. locks its memory and optionally switches to SCHED_FIFO
 *   2. wakes up every period for a number of seconds, using either
 *        abs: clock_nanosleep(TIMER_ABSTIME) until the next deadline, or
 *        rel: a relative sleep of one period
 *   3. stores how late each wake-up was (no printing inside the loop)
 *   4. prints min / avg / max, a histogram, and how far the schedule drifted
 *   5. optionally writes every sample to a CSV file for plot_latency.py
 *
 * Usage:  ./latency [priority] [seconds] [mode] [period-us] [csv-file]
 *   ./latency                    SCHED_FIFO 80, 5 s, abs, 1000 us
 *   ./latency 0 5                SCHED_OTHER, for comparison
 *   ./latency 80 5 rel           relative sleep: watch the drift line
 *   ./latency 80 10 abs 1000 fifo.csv
 *
 * Latency = actual wake-up time - intended wake-up time. Never negative.
 * Jitter  = how much the latency varies (max - min).
 */

#include "rt.h"

/* Count the samples per latency range and print one line per range. */
static void print_histogram(const long long samples[], long count) {
    /* Upper bound of each bucket, in microseconds. The last bucket is open. */
    const long long bounds_us[] = {10, 20, 50, 100, 200, 500, 1000, 5000};
    const int bounds = 8;
    long long counts[9] = {0};

    for (long i = 0; i < count; i++) {
        int b = 0;
        while (b < bounds && samples[i] >= bounds_us[b] * RT_NS_PER_US) b++;
        counts[b]++;
    }

    printf("histogram:\n");
    long long lower = 0;
    for (int b = 0; b < bounds; b++) {
        printf("  %5lld .. %5lld us  %8lld\n", lower, bounds_us[b], counts[b]);
        lower = bounds_us[b];
    }
    printf("  %5lld us and more %8lld\n", lower, counts[bounds]);
}

int main(int argc, char **argv) {
    int priority = (int)rt_arg_long(argc, argv, 1, 80, 0, 99);
    long seconds = rt_arg_long(argc, argv, 2, 5, 1, 3600);
    const char *mode = "abs";
    if (argc > 3) mode = argv[3];
    long period_us = rt_arg_long(argc, argv, 4, 1000, 100, 1000000);
    const char *csv = NULL;
    if (argc > 5) csv = argv[5];

    int absolute = strcmp(mode, "abs") == 0;
    if (!absolute && strcmp(mode, "rel") != 0) {
        fprintf(stderr, "mode must be 'abs' or 'rel'\n");
        return 2;
    }
    long long period_ns = period_us * RT_NS_PER_US;
    long iterations = seconds * 1000000L / period_us;

    /* Allocate all storage BEFORE the loop: malloc inside a real-time loop
     * can take an unpredictable amount of time. */
    long long *samples = malloc(iterations * sizeof(samples[0]));
    if (samples == NULL) {
        perror("malloc");
        return 1;
    }

    if (rt_lock_memory() != 0) return 1;
    if (rt_set_self_sched(priority) != 0) return 1;
    rt_print_self_sched("latency");
    printf("mode %s, period %ld us, %ld iterations\n", mode, period_us, iterations);

    struct timespec start = rt_now();
    struct timespec next = rt_add_ns(start, period_ns); /* the first intended wake-up */

    for (long i = 0; i < iterations; i++) {
        if (absolute) {
            /* Sleep until a fixed point in time. */
            rt_sleep_until(next);
        } else {
            /* Sleep "one period from now". */
            struct timespec one_period = {0, period_ns};
            clock_nanosleep(CLOCK_MONOTONIC, 0, &one_period, NULL);
        }

        struct timespec woke = rt_now();
        samples[i] = rt_diff_ns(woke, next); /* how late is this wake-up? */

        if (absolute) {
            /* The next deadline is computed from the previous DEADLINE, so
             * being late now does not move any later wake-up. */
            next = rt_add_ns(next, period_ns);
        } else {
            /* The next deadline is computed from when we actually WOKE, so
             * every late wake-up pushes all later ones back: drift. */
            next = rt_add_ns(woke, period_ns);
        }
    }

    /* Everything below runs after the time-critical part. */
    long long total_ns = rt_elapsed_ns(start);

    struct rt_stats stats;
    rt_stats_init(&stats);
    for (long i = 0; i < iterations; i++) rt_stats_add(&stats, samples[i]);
    rt_stats_print_us(&stats, "wake-up latency");
    printf("jitter (max - min): %lld us\n", (stats.max - stats.min) / RT_NS_PER_US);
    print_histogram(samples, iterations);
    printf("drift: %ld periods should take %.3f ms, took %.3f ms (%+.3f ms)\n", iterations,
           iterations * period_ns / 1e6, total_ns / 1e6,
           (total_ns - iterations * period_ns) / 1e6);

    if (csv != NULL) {
        FILE *out = fopen(csv, "w");
        if (out == NULL) {
            perror(csv);
            return 1;
        }
        fprintf(out, "# iteration latency_us  (priority %d, mode %s, period %ld us)\n", priority,
                mode, period_us);
        for (long i = 0; i < iterations; i++) {
            fprintf(out, "%ld %.3f\n", i, samples[i] / 1e3);
        }
        fclose(out);
        printf("samples written to %s\n", csv);
    }

    free(samples);
    return 0;
}
