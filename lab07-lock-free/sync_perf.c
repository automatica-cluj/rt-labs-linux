/*
 * sync_perf.c - mutex, priority-inheritance mutex and an atomic compared
 *
 * Two tests, each run for the three ways of protecting a shared counter:
 *
 *   1. throughput: several SCHED_OTHER threads increment the counter as fast
 *      as they can for a fixed time, on as many CPUs as they like. Measures
 *      the average cost of one increment.
 *
 *   2. mixed priority: three SCHED_FIFO threads pinned to ONE CPU.
 *        high   priority 50, every 5 ms, one short update of the counter
 *        medium priority 30, every 23 ms, 8 ms of pure computation
 *        low    priority 10, every 7 ms, a 3 ms update of the counter
 *      We record, for every period of the high thread, how long after its
 *      release it finished its update. That is its response time; the max is
 *      what a real-time designer cares about.
 *
 * Who can delay the high thread?
 *   plain mutex: low (it holds the lock), and medium too, because medium
 *                preempts low while low holds the lock. That is unbounded
 *                priority inversion.
 *   PI mutex:    only low, and only for the rest of its critical section:
 *                low runs at high's priority while high waits for the lock.
 *   atomic:      nobody. low does its 3 ms of work on its own and publishes
 *                the result in one atomic step, so high never waits.
 *
 * Usage:  ./sync_perf [threads] [seconds_per_method] [cpu]
 *   defaults: 4 threads, 2 s, a CPU picked from your uid
 */

#include "rt.h"

#include <stdatomic.h>
#include <stdbool.h>

#define MAX_THREADS 64

enum method { METHOD_MUTEX, METHOD_PI_MUTEX, METHOD_ATOMIC };

static const char *method_name(enum method m) {
    switch (m) {
        case METHOD_MUTEX: return "mutex";
        case METHOD_PI_MUTEX: return "PI mutex";
        default: return "atomic";
    }
}

/* One shared counter, protected in one of three ways. */
struct counter {
    enum method method;
    pthread_mutex_t mutex;
    long plain;          /* used with the two mutex methods */
    atomic_long atomic;  /* used with METHOD_ATOMIC */
};

static void counter_init(struct counter *c, enum method m) {
    c->method = m;
    c->plain = 0;
    atomic_init(&c->atomic, 0);
    if (rt_mutex_init(&c->mutex, m == METHOD_PI_MUTEX) != 0) exit(1);
}

/* Do `work_ns` of computation that belongs to the update, then add 1. */
static void counter_update(struct counter *c, long long work_ns) {
    if (c->method == METHOD_ATOMIC) {
        /* Compute outside, publish in one atomic step. Nobody ever waits. */
        if (work_ns > 0) rt_busy_ns(work_ns);
        atomic_fetch_add(&c->atomic, 1);
    } else {
        /* The computation is inside the critical section, as it would be if
         * it read and modified shared state. Everyone else waits meanwhile. */
        pthread_mutex_lock(&c->mutex);
        if (work_ns > 0) rt_busy_ns(work_ns);
        c->plain++;
        pthread_mutex_unlock(&c->mutex);
    }
}

static long counter_value(struct counter *c) {
    if (c->method == METHOD_ATOMIC) return atomic_load(&c->atomic);
    return c->plain;
}

/* --------------------------------------------------------------- test 1 ---- */

/*
 * _Alignas(64): each thread's struct starts on its own 64-byte cache line.
 * Without it, the `done` counters of neighbouring threads would share a cache
 * line and slow each other down (false sharing), which would distort the
 * comparison.
 */
struct bench {
    _Alignas(64) struct counter *counter;
    pthread_barrier_t *start;
    atomic_bool *stop;
    long done; /* increments made by this thread */
};

static void *bench_worker(void *arg) {
    struct bench *b = arg;
    pthread_barrier_wait(b->start);
    while (!atomic_load(b->stop)) {
        counter_update(b->counter, 0);
        b->done++;
    }
    return NULL;
}

/* Returns increments per second; *correct says whether any were lost. */
static double throughput(enum method m, int threads, int seconds, bool *correct) {
    static struct bench bench[MAX_THREADS];
    pthread_t ids[MAX_THREADS];
    struct counter counter;
    atomic_bool stop;
    pthread_barrier_t start;

    counter_init(&counter, m);
    atomic_init(&stop, false);
    pthread_barrier_init(&start, NULL, threads + 1);

    for (int t = 0; t < threads; t++) {
        bench[t].counter = &counter;
        bench[t].start = &start;
        bench[t].stop = &stop;
        bench[t].done = 0;
        if (rt_start_thread(&ids[t], bench_worker, &bench[t], 0, -1) != 0) exit(1);
    }

    pthread_barrier_wait(&start);
    struct timespec t0 = rt_now();
    rt_sleep_ms(seconds * 1000L);
    atomic_store(&stop, true);
    for (int t = 0; t < threads; t++) pthread_join(ids[t], NULL);
    double elapsed = rt_elapsed_ns(t0) / 1e9;

    long total = 0;
    for (int t = 0; t < threads; t++) total += bench[t].done;
    *correct = (counter_value(&counter) == total); /* no increment lost */

    pthread_barrier_destroy(&start);
    pthread_mutex_destroy(&counter.mutex);
    return total / elapsed;
}

/* --------------------------------------------------------------- test 2 ---- */

struct periodic {
    struct counter *counter; /* NULL: the thread only computes */
    long long period_ns;
    long long work_ns;
    atomic_bool *stop;
    struct rt_stats response; /* release -> end of work */
    long updates;
};

static void periodic_init(struct periodic *p, struct counter *counter, long long period_ns,
                          long long work_ns, atomic_bool *stop) {
    p->counter = counter;
    p->period_ns = period_ns;
    p->work_ns = work_ns;
    p->stop = stop;
    p->updates = 0;
    rt_stats_init(&p->response);
}

static void *periodic_worker(void *arg) {
    struct periodic *p = arg;
    struct timespec release = rt_now();
    while (!atomic_load(p->stop)) {
        if (p->counter != NULL) {
            counter_update(p->counter, p->work_ns);
            p->updates++;
        } else {
            rt_busy_ns(p->work_ns);
        }
        /* Response time: from the moment we should have started to now. */
        rt_stats_add(&p->response, rt_elapsed_ns(release));

        release = rt_add_ns(release, p->period_ns);
        /*
         * Overrun policy: if we are already past the next release, skip the
         * periods we missed instead of running back to back. Without this a
         * late low-priority thread would hog the CPU while catching up.
         */
        struct timespec t = rt_now();
        while (rt_diff_ns(release, t) < 0) release = rt_add_ns(release, p->period_ns);
        rt_sleep_until(release);
    }
    return NULL;
}

/* Fills *high_response; returns true if no counter update was lost. */
static bool mixed_priority(enum method m, int seconds, int cpu, struct rt_stats *high_response) {
    struct counter counter;
    atomic_bool stop;
    struct periodic high, medium, low;

    counter_init(&counter, m);
    atomic_init(&stop, false);
    periodic_init(&high, &counter, 5 * RT_NS_PER_MS, 50 * RT_NS_PER_US, &stop);
    periodic_init(&medium, NULL, 23 * RT_NS_PER_MS, 8 * RT_NS_PER_MS, &stop);
    periodic_init(&low, &counter, 7 * RT_NS_PER_MS, 3 * RT_NS_PER_MS, &stop);

    /* All three on the same CPU: priorities only compete on a shared CPU. */
    pthread_t th_low, th_medium, th_high;
    if (rt_start_thread(&th_low, periodic_worker, &low, 10, cpu) != 0 ||
        rt_start_thread(&th_medium, periodic_worker, &medium, 30, cpu) != 0 ||
        rt_start_thread(&th_high, periodic_worker, &high, 50, cpu) != 0) {
        fprintf(stderr, "this test needs SCHED_FIFO priorities 10..50\n");
        exit(1);
    }

    rt_sleep_ms(seconds * 1000L);
    atomic_store(&stop, true);
    pthread_join(th_high, NULL);
    pthread_join(th_medium, NULL);
    pthread_join(th_low, NULL);

    *high_response = high.response;
    bool correct = (counter_value(&counter) == high.updates + low.updates);
    pthread_mutex_destroy(&counter.mutex);
    return correct;
}

int main(int argc, char **argv) {
    const int threads = (int)rt_arg_long(argc, argv, 1, 4, 1, MAX_THREADS);
    const int seconds = (int)rt_arg_long(argc, argv, 2, 2, 1, 60);
    const int cpu = (int)rt_arg_long(argc, argv, 3, rt_default_cpu(), 0, CPU_SETSIZE - 1);
    const enum method methods[3] = {METHOD_MUTEX, METHOD_PI_MUTEX, METHOD_ATOMIC};
    bool all_correct = true;

    rt_lock_memory();

    printf("Test 1: throughput, %d SCHED_OTHER threads, %d s per method\n", threads, seconds);
    double best_rate = 0;
    enum method best = METHOD_MUTEX;
    for (int i = 0; i < 3; i++) {
        bool correct = false;
        double rate = throughput(methods[i], threads, seconds, &correct);
        if (!correct) all_correct = false;
        printf("  %-12s %7.2f M increments/s  count %s\n", method_name(methods[i]), rate / 1e6,
               correct ? "correct" : "WRONG");
        if (rate > best_rate) {
            best_rate = rate;
            best = methods[i];
        }
    }
    printf("  highest throughput here: %s\n\n", method_name(best));

    printf("Test 2: mixed priorities on CPU %d, %d s per method\n", cpu, seconds);
    printf("  high: FIFO 50, 5 ms period, 50 us update\n");
    printf("  medium: FIFO 30, 23 ms period, 8 ms computation (no counter)\n");
    printf("  low: FIFO 10, 7 ms period, 3 ms update\n");
    long long best_max = -1;
    long long max_by_method[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        struct rt_stats high;
        bool correct = mixed_priority(methods[i], seconds, cpu, &high);
        if (!correct) all_correct = false;
        char label[64];
        snprintf(label, sizeof(label), "  %-12s high response", method_name(methods[i]));
        rt_stats_print_us(&high, label);
        if (!correct) printf("  %-12s count WRONG\n", method_name(methods[i]));
        max_by_method[i] = high.max;
        if (best_max < 0 || high.max < best_max) {
            best_max = high.max;
            best = methods[i];
        }
    }

    /* The conclusions below are computed from this run's numbers. */
    printf("\nWhat the numbers say on this run:\n");
    printf("  lowest worst-case response for high: %s (%lld us)\n", method_name(best),
           best_max / RT_NS_PER_US);
    double ratio = (double)max_by_method[0] / (double)max_by_method[1];
    if (ratio > 1.5) {
        printf("  plain mutex worst case is %.1fx the PI mutex worst case: priority\n"
               "  inversion through the medium thread was observed.\n", ratio);
    } else {
        printf("  plain mutex and PI mutex worst cases are close (ratio %.1f): the bad\n"
               "  interleaving did not happen in this run. Run longer.\n", ratio);
    }

    if (all_correct) return 0;
    return 1;
}
