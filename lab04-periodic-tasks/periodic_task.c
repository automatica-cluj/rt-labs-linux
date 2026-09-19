/*
 * periodic_task.c - one periodic task with a deadline
 *
 * A control loop is released every `period`, computes for `wcet`, and must be
 * finished before its next release (implicit deadline = period). The program:
 *   1. locks its memory and pins itself to one CPU
 *   2. switches to SCHED_FIFO at the given priority (0 = SCHED_OTHER)
 *   3. runs the loop with an absolute timer, recording for every job:
 *        release latency   how late the job started   (wake-up - release)
 *        response time     how long until it finished (completion - release)
 *        deadline miss     response time > period
 *   4. prints a summary at the end. Nothing is printed inside the loop:
 *      printf can block on the terminal and cause the misses we measure.
 *
 * Overrun policy: if a job finishes after one or more later releases have
 * already passed, those releases are skipped (counted, not run late) and the
 * task re-synchronises with the next release still in the future. Without
 * this, a late task runs a burst of back-to-back catch-up jobs.
 *
 * Usage:  ./periodic_task [priority] [seconds] [cpu] [period_us] [wcet_us]
 *   defaults: 80 5 <per-user cpu> 10000 2000   (cpu -1 = do not pin)
 */

#include "rt.h"

/* Job numbers of the first few misses are kept in a fixed array. Its size is
 * known before the loop starts, so the loop never allocates memory. */
#define MAX_RECORDED_MISSES 10

int main(int argc, char **argv) {
    int priority = (int)rt_arg_long(argc, argv, 1, 80, 0, 99);
    int seconds = (int)rt_arg_long(argc, argv, 2, 5, 1, 3600);
    int cpu = (int)rt_arg_long(argc, argv, 3, rt_default_cpu(), -1, CPU_SETSIZE - 1);
    long period_us = rt_arg_long(argc, argv, 4, 10000, 100, 1000000);
    long wcet_us = rt_arg_long(argc, argv, 5, 2000, 0, 1000000);

    long long period_ns = period_us * RT_NS_PER_US;
    long long wcet_ns = wcet_us * RT_NS_PER_US;

    /* Prepare: no page faults, one CPU, real-time priority. */
    if (rt_lock_memory() != 0) return 1;
    if (cpu >= 0 && rt_pin_self_to_cpu(cpu) != 0) return 1;
    if (rt_set_self_sched(priority) != 0) return 1;

    rt_print_self_sched("periodic_task");
    printf("period %ld us, computation %ld us, utilisation %.1f %%, %d s\n", period_us, wcet_us,
           100.0 * wcet_us / period_us, seconds);

    /* Results, filled in by the loop and printed after it. */
    struct rt_stats latency;
    struct rt_stats response_time;
    rt_stats_init(&latency);
    rt_stats_init(&response_time);
    long jobs = 0;
    long misses = 0;
    long skipped = 0;
    long miss_jobs[MAX_RECORDED_MISSES];

    /* The first release is one period from now; the run ends `seconds` later. */
    struct timespec release = rt_add_ns(rt_now(), period_ns);
    struct timespec end = rt_add_ns(release, seconds * RT_NS_PER_SEC);

    while (rt_diff_ns(release, end) < 0) {
        /* 1. Wait for the release. Absolute time, so the period never drifts. */
        rt_sleep_until(release);

        /* 2. Release latency: how late did we actually start? */
        struct timespec woke = rt_now();
        rt_stats_add(&latency, rt_diff_ns(woke, release));

        /* 3. The job itself: the "control algorithm". */
        rt_busy_ns(wcet_ns);

        /* 4. Response time, and the deadline check (deadline = next release). */
        struct timespec done = rt_now();
        long long response = rt_diff_ns(done, release);
        rt_stats_add(&response_time, response);
        if (response > period_ns) {
            if (misses < MAX_RECORDED_MISSES) miss_jobs[misses] = jobs;
            misses++;
        }
        jobs++;

        /* 5. Next release. If we are so late that it has passed too, skip the
         *    releases that are already in the past (the overrun policy). */
        release = rt_add_ns(release, period_ns);
        long long behind = rt_diff_ns(done, release);
        if (behind > 0) {
            long long lost = behind / period_ns + 1;
            skipped += lost;
            release = rt_add_ns(release, lost * period_ns);
        }
    }

    /* Report. Everything below runs after the time-critical part. */
    printf("\njobs run %ld, releases skipped %ld\n", jobs, skipped);
    rt_stats_print_us(&latency, "release latency");
    rt_stats_print_us(&response_time, "response time  ");

    double miss_percent = 0.0;
    if (jobs > 0) miss_percent = 100.0 * misses / jobs;
    printf("deadline misses: %ld of %ld jobs (%.2f %%)\n", misses, jobs, miss_percent);

    if (misses > 0) {
        printf("first missed jobs:");
        for (long i = 0; i < misses && i < MAX_RECORDED_MISSES; i++) {
            printf(" %ld", miss_jobs[i]);
        }
        printf("\n");
        printf("RESULT: deadline misses (worst response %lld us > period %ld us)\n",
               response_time.max / RT_NS_PER_US, period_us);
    } else {
        printf("RESULT: all deadlines met (worst response %lld us, slack %lld us)\n",
               response_time.max / RT_NS_PER_US, (period_ns - response_time.max) / RT_NS_PER_US);
    }
    return 0;
}
