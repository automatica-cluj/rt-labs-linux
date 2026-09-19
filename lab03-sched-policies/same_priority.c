/*
 * same_priority.c - FIFO versus RR versus OTHER when priorities are equal
 *
 * Three CPU-bound workers A, B, C with the SAME policy and priority, pinned to
 * one CPU and released in the order A, B, C (see timeline.h).
 *
 *   fifo   the first one runs to completion, then the next
 *   rr     each runs for one time slice (usually 100 ms, see
 *          /proc/sys/kernel/sched_rr_timeslice_ms), then goes to the back
 *          of the queue
 *   other  the fair scheduler switches every few milliseconds
 *
 * Usage:  ./same_priority [fifo|rr|other] [priority] [cpu] [work_ms]
 *   priority  1..79 for fifo/rr, ignored for other. Default 50.
 *   cpu       CPU to pin to, -1 for no pinning. Default: derived from your user id.
 *   work_ms   CPU time per worker. Default 150.
 */

#include "timeline.h"

#define WORKERS 3

/* The SCHED_RR time slice is a kernel setting. Show it if we can read it. */
static void print_rr_timeslice(void) {
    FILE *f = fopen("/proc/sys/kernel/sched_rr_timeslice_ms", "r");
    if (f == NULL) return;
    int ms = 0;
    if (fscanf(f, "%d", &ms) == 1) printf("SCHED_RR time slice: %d ms\n", ms);
    fclose(f);
}

int main(int argc, char **argv) {
    const char *mode = "fifo";
    if (argc > 1) mode = argv[1];

    int policy;
    if (strcmp(mode, "fifo") == 0) {
        policy = SCHED_FIFO;
    } else if (strcmp(mode, "rr") == 0) {
        policy = SCHED_RR;
    } else if (strcmp(mode, "other") == 0) {
        policy = SCHED_OTHER;
    } else {
        fprintf(stderr, "usage: %s [fifo|rr|other] [priority] [cpu] [work_ms]\n", argv[0]);
        return 2;
    }

    int prio = (int)rt_arg_long(argc, argv, 2, 50, 1, 79);
    if (policy == SCHED_OTHER) prio = 0;
    int cpu = (int)rt_arg_long(argc, argv, 3, rt_default_cpu(), -1, CPU_SETSIZE - 1);
    long work_ms = rt_arg_long(argc, argv, 4, 150, 1, 300);

    /* Main runs one step above the workers, so they wait for main. */
    int prio_main = 0;
    if (prio > 0) prio_main = prio + 1;
    setup_main(prio_main, cpu);

    if (policy == SCHED_RR) print_rr_timeslice();

    /* Same policy, same priority: only the policy's own rule decides. */
    struct worker w[WORKERS];
    worker_setup(&w[0], "A", 'A', policy, prio, cpu, work_ms);
    worker_setup(&w[1], "B", 'B', policy, prio, cpu, work_ms);
    worker_setup(&w[2], "C", 'C', policy, prio, cpu, work_ms);

    printf("3 workers x %ld ms of CPU time, %s priority %d, %s\n", work_ms,
           rt_policy_name(policy), prio, cpu_text(cpu));
    start_all(w, WORKERS);
    report(w, WORKERS);

    if (parallel(w, WORKERS)) {
        printf("result: workers ran in parallel\n");
    } else if (interleaved(w, WORKERS)) {
        printf("result: workers took turns (time slicing)\n");
    } else {
        printf("result: one after another, each ran to completion\n");
    }
    return 0;
}
