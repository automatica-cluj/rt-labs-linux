/*
 * time_basics.c - reading and subtracting high-resolution timestamps
 *
 * The program:
 *   1. reads CLOCK_MONOTONIC and prints the raw timespec
 *   2. converts an interval to ns, us, ms and s
 *   3. sleeps for several durations and shows how late each sleep ends
 *
 * Usage:  ./time_basics
 *
 * CLOCK_MONOTONIC never jumps (NTP, date changes). CLOCK_REALTIME is
 * wall-clock time and can jump backwards, so never use it to measure
 * intervals.
 */

#define _GNU_SOURCE /* must come before the includes: enables clock_gettime and nanosleep */

#include <stdio.h>
#include <time.h>

#define NS_PER_SEC 1000000000LL

/* A timespec has two fields: whole seconds, and the nanoseconds past them. */
static long long to_ns(struct timespec t) {
    return t.tv_sec * NS_PER_SEC + t.tv_nsec;
}

/*
 * (a - b) in nanoseconds. tv_nsec may go negative in the subtraction; the
 * seconds part makes up for it, so no special case is needed.
 */
static long long diff_ns(struct timespec a, struct timespec b) {
    return (a.tv_sec - b.tv_sec) * NS_PER_SEC + (a.tv_nsec - b.tv_nsec);
}

/* Relative sleep with nanosecond resolution. */
static void sleep_ns(long long ns) {
    struct timespec req;
    req.tv_sec = ns / NS_PER_SEC;
    req.tv_nsec = ns % NS_PER_SEC;
    nanosleep(&req, NULL);
}

int main(void) {
    struct timespec start;
    struct timespec end;

    /* 1. One timestamp. The absolute value is "time since boot"; only the
     *    difference between two readings is meaningful. */
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        return 1;
    }
    printf("1. now = %lld s + %ld ns  (= %lld ns since boot)\n", (long long)start.tv_sec,
           start.tv_nsec, to_ns(start));

    /* 2. An interval, in different units. */
    sleep_ns(100 * 1000000LL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    long long elapsed = diff_ns(end, start);
    printf("\n2. slept 100 ms, measured:\n");
    printf("   %lld ns = %.1f us = %.3f ms = %.6f s\n", elapsed, elapsed / 1e3, elapsed / 1e6,
           elapsed / 1e9);

    /* 3. Sleeps always end late, never early. How late depends on the
     *    scheduler, the load, the timer slack and the kernel. */
    const long targets_ms[] = {1, 5, 10, 50};
    printf("\n3. requested vs measured sleep\n");
    printf("   target ms | measured ms | late by us\n");
    for (int i = 0; i < 4; i++) {
        long long target_ns = targets_ms[i] * 1000000LL;
        clock_gettime(CLOCK_MONOTONIC, &start);
        sleep_ns(target_ns);
        clock_gettime(CLOCK_MONOTONIC, &end);
        long long measured = diff_ns(end, start);
        printf("   %9ld | %11.3f | %10.1f\n", targets_ms[i], measured / 1e6,
               (measured - target_ns) / 1e3);
    }
    return 0;
}
