/*
 * policy_mix.c - SCHED_OTHER, SCHED_RR and SCHED_FIFO competing for one CPU
 *
 * The same amount of work under three policies, all pinned to one CPU:
 *   O  SCHED_OTHER
 *   R  SCHED_RR   at `priority`
 *   F  SCHED_FIFO at `priority`   (same priority as R)
 * They are released in the order O, R, F.
 *
 * Things to look for in the output:
 *   - O does (almost) nothing while a real-time task is runnable.
 *   - R and F have the same priority. R was queued first, so it starts, but
 *     after one RR time slice it goes to the back of the queue, behind F. F is
 *     FIFO: it is never sliced and keeps the CPU until it is done.
 *   - Since Linux 6.12 the "fair server" (a SCHED_DEADLINE reservation) still
 *     gives normal tasks up to 50 ms per second, so O may get a few pieces in.
 *
 * Usage:  ./policy_mix [priority] [cpu] [work_ms]
 *   priority  1..79 for R and F. Default 50.
 *   cpu       CPU to pin to, -1 for no pinning. Default: derived from your user id.
 *   work_ms   CPU time per worker, up to 600. Default 150.
 */

#include "timeline.h"

#define WORKERS 3

int main(int argc, char **argv) {
    int prio = (int)rt_arg_long(argc, argv, 1, 50, 1, 79);
    int cpu = (int)rt_arg_long(argc, argv, 2, rt_default_cpu(), -1, CPU_SETSIZE - 1);
    long work_ms = rt_arg_long(argc, argv, 3, 150, 1, 600);

    /* Main runs one step above the workers, so they wait for main. */
    setup_main(prio + 1, cpu);

    struct worker w[WORKERS];
    worker_setup(&w[0], "other", 'O', SCHED_OTHER, 0, cpu, work_ms);
    worker_setup(&w[1], "rr", 'R', SCHED_RR, prio, cpu, work_ms);
    worker_setup(&w[2], "fifo", 'F', SCHED_FIFO, prio, cpu, work_ms);

    printf("3 workers x %ld ms of CPU time on %s, released in order O R F\n", work_ms,
           cpu_text(cpu));
    start_all(w, WORKERS);
    report(w, WORKERS);

    /* Any real-time task beats any normal task, so O should finish last. */
    if (w[0].finish_ns > w[1].finish_ns && w[0].finish_ns > w[2].finish_ns) {
        printf("result: SCHED_OTHER finished last, after both real-time workers\n");
    } else {
        printf("result: SCHED_OTHER finished BEFORE a real-time worker\n");
    }
    return 0;
}
