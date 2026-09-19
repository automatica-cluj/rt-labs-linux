/*
 * hello_rt.c - first contact with a PREEMPT_RT kernel
 *
 * The program:
 *   1. locks its memory so no page fault can happen mid-loop
 *   2. moves itself to SCHED_FIFO at the priority given on the command line
 *   3. wakes up every 1 ms for a few seconds using an absolute timer
 *   4. reports how late each wake-up was (min / avg / max, in microseconds)
 *
 * Usage:  ./hello_rt [priority] [seconds]
 *   priority 0      stay a normal (SCHED_OTHER) task, for comparison
 *   priority 1..99  SCHED_FIFO at that priority (students are capped at 80)
 *
 * Everything here is plain POSIX C. There is no special "real-time language":
 * real-time behaviour comes from how you ask the kernel to treat your task.
 */

#define _GNU_SOURCE /* must come before the includes: enables the POSIX/Linux calls below */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define NS_PER_SEC 1000000000L
#define PERIOD_NS 1000000L /* 1 ms */

/* Advance a timespec by ns, keeping tv_nsec below one second. */
static void add_ns(struct timespec *t, long ns) {
    t->tv_nsec += ns;
    while (t->tv_nsec >= NS_PER_SEC) {
        t->tv_nsec -= NS_PER_SEC;
        t->tv_sec += 1;
    }
}

/* (a - b) in nanoseconds. */
static long diff_ns(struct timespec a, struct timespec b) {
    return (a.tv_sec - b.tv_sec) * NS_PER_SEC + (a.tv_nsec - b.tv_nsec);
}

static const char *policy_name(int policy) {
    switch (policy) {
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_RR: return "SCHED_RR";
        case SCHED_OTHER: return "SCHED_OTHER";
        default: return "other";
    }
}

int main(int argc, char **argv) {
    int priority = 80;
    int seconds = 5;
    if (argc > 1) priority = atoi(argv[1]);
    if (argc > 2) seconds = atoi(argv[2]);
    long iterations = seconds * (NS_PER_SEC / PERIOD_NS);

    /* 1. Lock all current and future pages in RAM. A page fault inside the
     *    loop would cost far more than the period we are trying to hold. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall");
        fprintf(stderr, "hint: check 'ulimit -l' (memlock limit)\n");
        return 1;
    }

    /* 2. Ask the kernel for a real-time policy. A normal user needs
     *    RLIMIT_RTPRIO >= priority for this to succeed (see 'ulimit -r').
     *    pthread functions return the error number; they do not set errno. */
    if (priority > 0) {
        struct sched_param sp;
        memset(&sp, 0, sizeof(sp));
        sp.sched_priority = priority;
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        if (rc != 0) {
            fprintf(stderr, "pthread_setschedparam(SCHED_FIFO, %d): %s\n", priority, strerror(rc));
            if (rc == EPERM) {
                fprintf(stderr, "hint: 'ulimit -r' shows the highest priority you may use\n");
            }
            return 1;
        }
    }

    /* Read back what the kernel gave us. Never trust what you asked for. */
    int policy = 0;
    struct sched_param current;
    pthread_getschedparam(pthread_self(), &policy, &current);
    printf("running as %s priority %d, period %ld us, %d s\n", policy_name(policy),
           current.sched_priority, PERIOD_NS / 1000, seconds);

    /* 3. Periodic loop with an absolute deadline. TIMER_ABSTIME means the
     *    schedule never drifts: each wake-up is computed from the previous
     *    deadline, not from "now". */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    add_ns(&next, PERIOD_NS);

    long min_ns = NS_PER_SEC;
    long max_ns = 0;
    long sum_ns = 0;

    for (long i = 0; i < iterations; i++) {
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        /* How late did we wake up compared to the deadline we asked for? */
        long late = diff_ns(now, next);
        if (late < min_ns) min_ns = late;
        if (late > max_ns) max_ns = late;
        sum_ns += late;

        add_ns(&next, PERIOD_NS);
    }

    /* 4. Report. Real-time is about the worst case: read the max, not the avg. */
    printf("wake-up latency over %ld iterations:\n", iterations);
    printf("  min %6ld us\n", min_ns / 1000);
    printf("  avg %6ld us\n", sum_ns / iterations / 1000);
    printf("  max %6ld us\n", max_ns / 1000);
    return 0;
}
