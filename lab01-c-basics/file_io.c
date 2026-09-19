/*
 * file_io.c - store measurements in structs, write them to a data file
 *
 * The program:
 *   1. takes 10 timestamps a random 10..100 ms apart and stores them
 *   2. writes them to a text file that Python or gnuplot can read
 *   3. computes min / avg / max from the stored data
 *
 * Usage:  ./file_io [output-file]      (default: measurements.txt)
 *
 * Collect first, write afterwards. Writing to a file inside a timing loop
 * adds unpredictable delays to exactly what you are trying to measure.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define COUNT 10

struct measurement {
    int index;
    double at_ms;       /* time since the start */
    double interval_ms; /* time since the previous measurement */
};

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    const char *path = "measurements.txt";
    if (argc > 1) path = argv[1];

    /* 1. Measure. The array exists before the loop starts, so the loop itself
     *    only sleeps, reads the clock and stores a value. */
    struct measurement data[COUNT];
    srand(1); /* fixed seed: the same sleep pattern on every run */
    double start = now_ms();
    double prev = start;
    for (int i = 0; i < COUNT; i++) {
        long ms = 10 + rand() % 91; /* 10..100 ms */
        struct timespec req = {0, ms * 1000000L};
        nanosleep(&req, NULL);

        double t = now_ms();
        data[i].index = i;
        data[i].at_ms = t - start;
        data[i].interval_ms = t - prev;
        prev = t;
    }

    /* 2. Write. Header lines start with '#' so plotting tools skip them. */
    FILE *out = fopen(path, "w");
    if (out == NULL) {
        perror(path); /* fopen does set errno, so perror is right here */
        return 1;
    }
    fprintf(out, "# index at_ms interval_ms\n");
    for (int i = 0; i < COUNT; i++) {
        fprintf(out, "%d %.3f %.3f\n", data[i].index, data[i].at_ms, data[i].interval_ms);
    }
    if (fclose(out) != 0) {
        perror(path);
        return 1;
    }
    printf("wrote %d measurements to %s\n", COUNT, path);

    /* 3. Summarise from the stored data. */
    double min = data[0].interval_ms;
    double max = data[0].interval_ms;
    double sum = 0;
    for (int i = 0; i < COUNT; i++) {
        if (data[i].interval_ms < min) min = data[i].interval_ms;
        if (data[i].interval_ms > max) max = data[i].interval_ms;
        sum += data[i].interval_ms;
    }
    printf("interval: min %.2f ms, avg %.2f ms, max %.2f ms\n", min, sum / COUNT, max);
    return 0;
}
