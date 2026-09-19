/*
 * fifo_order.c - strict priority order under SCHED_FIFO
 *
 * Three CPU-bound workers, High, Medium and Low, all pinned to one CPU. They
 * are released by main in the order Low, Medium, High. Main runs one priority
 * step above them, so none can start before main blocks in pthread_join. At
 * that moment all three are runnable and the scheduler picks.
 *
 *   SCHED_FIFO: the highest priority runs to completion, then the next one.
 *               Each worker runs in one piece.
 *   SCHED_OTHER (priority 0): the fair scheduler interleaves all three.
 *
 * Usage:  ./fifo_order [priority] [cpu] [work_ms]
 *   priority  priority of High (21..79); Medium = priority-10, Low = priority-20.
 *             0 -> everything SCHED_OTHER, for comparison. Default 70.
 *   cpu       CPU to pin to, -1 for no pinning. Default: derived from your user id.
 *   work_ms   CPU time each worker burns. Default 150.
 */

#include "timeline.h"

#define WORKERS 3

int main(int argc, char **argv) {
    int prio = (int)rt_arg_long(argc, argv, 1, 70, 0, 79);
    if (prio != 0 && prio < 21) {
        fprintf(stderr, "priority must be 0 or in [21, 79]\n");
        return 2;
    }
    int cpu = (int)rt_arg_long(argc, argv, 2, rt_default_cpu(), -1, CPU_SETSIZE - 1);
    long work_ms = rt_arg_long(argc, argv, 3, 150, 1, 300);

    /* Priority 0 means: run everything under the normal, fair scheduler. */
    int policy = SCHED_OTHER;
    int prio_low = 0, prio_medium = 0, prio_high = 0, prio_main = 0;
    if (prio > 0) {
        policy = SCHED_FIFO;
        prio_low = prio - 20;
        prio_medium = prio - 10;
        prio_high = prio;
        prio_main = prio + 1; /* one step above High: the workers wait for main */
    }
    setup_main(prio_main, cpu);

    /* Released lowest priority first: if release order decided anything,
     * Low would win. Under SCHED_FIFO priority wins instead. */
    struct worker w[WORKERS];
    worker_setup(&w[0], "Low", 'L', policy, prio_low, cpu, work_ms);
    worker_setup(&w[1], "Medium", 'M', policy, prio_medium, cpu, work_ms);
    worker_setup(&w[2], "High", 'H', policy, prio_high, cpu, work_ms);

    printf("3 workers x %ld ms of CPU time on %s, released in order L M H\n", work_ms,
           cpu_text(cpu));
    start_all(w, WORKERS);
    report(w, WORKERS);

    if (prio == 0) {
        printf("result: SCHED_OTHER, the fair scheduler shared the CPU\n");
        return 0;
    }

    const char *order_text = "NOT in priority order";
    if (finished_in_order(w, WORKERS, "H M L")) order_text = "finished in priority order";

    const char *how_text = "one after another";
    if (parallel(w, WORKERS)) {
        how_text = "workers ran in parallel";
    } else if (interleaved(w, WORKERS)) {
        how_text = "workers took turns";
    }
    printf("result: %s, %s\n", order_text, how_text);
    return 0;
}
