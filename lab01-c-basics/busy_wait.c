/*
 * busy_wait.c - command-line arguments and CPU-burning work
 *
 * Real tasks compute, they do not only sleep. The labs simulate computation
 * with a busy loop that runs for a given time. This program runs such a loop
 * several times and reports how close to the target each run was.
 *
 * Usage:  ./busy_wait [duration-ms] [iterations]
 *   ./busy_wait            100 ms, 3 times
 *   ./busy_wait 20 10      20 ms, 10 times
 *
 * Watch it in 'htop' in a second terminal: one CPU at 100 %.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

/*
 * Read argv[index] as an integer in [lo, hi], or return fallback if the
 * argument is missing. atoi() would silently turn "abc" into 0 and accept
 * "-5"; strtol lets us reject both.
 */
static long parse(int argc, char **argv, int index, long fallback, long lo, long hi) {
    if (argc <= index) return fallback;
    char *end = NULL;
    errno = 0;
    long v = strtol(argv[index], &end, 10);
    if (errno != 0 || end == argv[index] || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "argument %d ('%s') must be an integer in [%ld, %ld]\n", index,
                argv[index], lo, hi);
        exit(2);
    }
    return v;
}

/*
 * Spin until duration_ns has passed. 'volatile' stops the optimiser from
 * deleting the loop body, whose result is never used.
 */
static void busy_wait(long long duration_ns) {
    long long start = now_ns();
    volatile unsigned long x = 0;
    while (now_ns() - start < duration_ns) {
        for (int i = 0; i < 10000; i++) x += i;
    }
}

int main(int argc, char **argv) {
    /* argv[0] is the program name; the user's arguments start at argv[1]. */
    long duration_ms = parse(argc, argv, 1, 100, 1, 10000);
    long iterations = parse(argc, argv, 2, 3, 1, 1000);
    printf("busy-waiting %ld ms, %ld times\n", duration_ms, iterations);

    printf("iter | measured ms | over by us\n");
    long long total_over = 0;
    for (long i = 0; i < iterations; i++) {
        long long t0 = now_ns();
        busy_wait(duration_ms * 1000000LL);
        long long took = now_ns() - t0;
        long long over = took - duration_ms * 1000000LL;
        total_over += over;
        printf("%4ld | %11.3f | %10.1f\n", i, took / 1e6, over / 1e3);
    }
    printf("average overrun: %.1f us\n", total_over / 1e3 / iterations);
    return 0;
}
