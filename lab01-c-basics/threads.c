/*
 * threads.c - creating threads, passing data in, getting results out
 *
 * The program starts three worker threads with pthread_create, waits for
 * them with pthread_join, and prints what each one computed.
 *
 * Usage:  ./threads
 *
 * Every real-time lab uses this pattern: one struct per thread carries the
 * thread's inputs and its results.
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define NUM_THREADS 3

/*
 * Everything a worker needs, and room for its result. Each thread gets its
 * OWN struct, so no two threads write the same memory and no lock is needed.
 */
struct work {
    int id;
    long amount;   /* input: how many numbers to add up */
    long long sum; /* output */
};

/*
 * The thread function. pthread entry points must have exactly this shape:
 * they take one void pointer and return one void pointer.
 */
static void *worker(void *arg) {
    struct work *w = arg; /* the pointer we passed to pthread_create */

    long long sum = 0;
    for (long i = 0; i < w->amount; i++) sum += i;
    w->sum = sum;

    return NULL;
}

int main(void) {
    pthread_t threads[NUM_THREADS];
    struct work work[NUM_THREADS];

    /* 1. Start the threads. */
    for (int i = 0; i < NUM_THREADS; i++) {
        work[i].id = i;
        work[i].amount = (i + 1) * 1000000L;
        work[i].sum = 0;

        /* pthread functions RETURN the error number. They do not set errno,
         * so perror() would print the wrong message. Use strerror(rc). */
        int rc = pthread_create(&threads[i], NULL, worker, &work[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            return 1;
        }
    }

    /* 2. Wait for each one to finish. Only after pthread_join returns is it
     *    safe to read that thread's result. */
    for (int i = 0; i < NUM_THREADS; i++) {
        int rc = pthread_join(threads[i], NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_join: %s\n", strerror(rc));
            return 1;
        }
    }

    /* 3. Results. */
    for (int i = 0; i < NUM_THREADS; i++) {
        printf("thread %d: sum of 0..%ld = %lld\n", work[i].id, work[i].amount - 1, work[i].sum);
    }
    return 0;
}
