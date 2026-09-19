/*
 * timeline.h - run CPU-bound workers and record WHEN each one ran
 *
 * All three programs in this lab use the same trick. A worker burns a fixed
 * amount of CPU time (measured with rt_thread_cpu_ns(), so waiting does not
 * count as work). Every few microseconds it looks at the clock. If more than
 * GAP_NS passed since the last look, somebody else had the CPU: the worker
 * closes its current "piece" of execution and starts a new one.
 *
 * After all workers finish, main prints:
 *   - requested versus obtained policy and priority (never trust the request)
 *   - first run, finish time and number of pieces per worker
 *   - a text strip showing which worker owned the CPU over time
 *
 * All workers are pinned to ONE CPU. On a multi-core machine unpinned threads
 * simply run in parallel on different CPUs and no scheduling policy has
 * anything to decide. Scheduling policies only matter when tasks compete for
 * the same CPU. Passing cpu -1 turns pinning off, so you can see that.
 *
 * The first half of this file is the real-time part (the worker, and how main
 * releases the workers). The second half only sorts and prints results.
 */

#ifndef TIMELINE_H
#define TIMELINE_H

#include "rt.h"

#include <semaphore.h>
#include <stdbool.h>

#define GAP_NS (500 * RT_NS_PER_US) /* a pause this long means: preempted */
#define MAX_PIECES 512
#define STRIP_COLS 64

/* One uninterrupted stretch of execution, in ns since the release. */
struct piece {
    long long start_ns;
    long long end_ns;
};

struct worker {
    /* set by main before the worker starts */
    const char *name;
    char tag;          /* the letter shown in the timeline strip */
    int policy;        /* SCHED_OTHER, SCHED_FIFO or SCHED_RR */
    int priority;
    int cpu;           /* CPU to pin to, -1 for no pinning */
    long long work_ns; /* CPU time to burn */

    /* used to park and release the worker */
    struct timespec t0; /* common time origin, set just before release */
    sem_t *parked;      /* the worker posts here when it is ready */
    sem_t go;           /* main posts here to release the worker */
    pthread_t thread;

    /* filled in by the worker itself */
    int got_policy;
    int got_priority;
    int got_cpu;
    struct piece pieces[MAX_PIECES];
    int npieces;
    long long first_run_ns;
    long long finish_ns;
};

/* Fill in what main decides about a worker. */
static inline void worker_setup(struct worker *w, const char *name, char tag, int policy,
                                int priority, int cpu, long work_ms) {
    memset(w, 0, sizeof(*w));
    w->name = name;
    w->tag = tag;
    w->policy = policy;
    w->priority = priority;
    w->cpu = cpu;
    w->work_ns = work_ms * RT_NS_PER_MS;
}

/* sem_wait can return early when a signal arrives. Wait again in that case. */
static inline void wait_for(sem_t *sem) {
    while (sem_wait(sem) != 0) {
    }
}

/*
 * Thread body. No printf in here: output to a terminal can block and would
 * change the very scheduling we are trying to observe.
 */
static inline void *worker_main(void *arg) {
    struct worker *w = arg;

    /* Read back what the kernel gave us, not what we asked for. */
    struct sched_param sp;
    pthread_getschedparam(pthread_self(), &w->got_policy, &sp);
    w->got_priority = sp.sched_priority;
    w->got_cpu = sched_getcpu();

    /* Park until main releases us. Waking a blocked task puts it at the
     * back of the queue for its priority, so the release order is known. */
    sem_post(w->parked);
    wait_for(&w->go);

    long long cpu_start = rt_thread_cpu_ns();
    long long last = rt_diff_ns(rt_now(), w->t0);
    w->first_run_ns = last;
    w->pieces[0].start_ns = last;
    w->npieces = 1;

    volatile unsigned long x = 0;
    while (rt_thread_cpu_ns() - cpu_start < w->work_ns) {
        for (int i = 0; i < 2000; i++) x += i; /* about 1-2 us of "work" */

        /* A long pause since the last look: we were preempted. */
        long long t = rt_diff_ns(rt_now(), w->t0);
        if (t - last > GAP_NS && w->npieces < MAX_PIECES) {
            w->pieces[w->npieces - 1].end_ns = last;
            w->pieces[w->npieces].start_ns = t;
            w->npieces++;
        }
        last = t;
    }
    w->pieces[w->npieces - 1].end_ns = last;
    w->finish_ns = last;
    return NULL;
}

/*
 * Main thread: optionally become a real-time task one step above the workers
 * on the same CPU. Workers cannot run while main is running, so main controls
 * exactly when and in which order they become runnable.
 */
static inline void setup_main(int priority, int cpu) {
    if (rt_lock_memory() != 0) exit(1);
    if (cpu >= 0 && rt_pin_self_to_cpu(cpu) != 0) exit(1);
    if (priority > 0 && rt_set_self_sched(priority) != 0) exit(1);
    rt_print_self_sched("main");
}

/*
 * Create all workers, wait until every one is parked, then release them in
 * array order and wait for them to finish.
 */
static inline void start_all(struct worker *workers, int count) {
    sem_t parked;
    sem_init(&parked, 0, 0);

    for (int i = 0; i < count; i++) {
        struct worker *w = &workers[i];
        w->parked = &parked;
        sem_init(&w->go, 0, 0);
        /* Explicit scheduling attributes: the thread is real-time from its
         * very first instruction. A failure (EPERM) is fatal on purpose. */
        if (rt_start_thread_policy(&w->thread, worker_main, w, w->policy, w->priority,
                                   w->cpu) != 0) {
            exit(1);
        }
    }
    for (int i = 0; i < count; i++) wait_for(&parked);

    /* Release. Main keeps the CPU until it blocks in pthread_join, so all
     * workers become runnable before any of them starts working. */
    struct timespec t0 = rt_now();
    for (int i = 0; i < count; i++) workers[i].t0 = t0;
    for (int i = 0; i < count; i++) sem_post(&workers[i].go);
    for (int i = 0; i < count; i++) pthread_join(workers[i].thread, NULL);

    for (int i = 0; i < count; i++) sem_destroy(&workers[i].go);
    sem_destroy(&parked);
}

/* ------------------------------------------------------------------------
 * From here on: sorting and printing only. Nothing below is time-critical.
 * ------------------------------------------------------------------------ */

static inline const char *short_policy(int policy) {
    switch (policy) {
        case SCHED_FIFO: return "FIFO";
        case SCHED_RR: return "RR";
        case SCHED_OTHER: return "OTHER";
        default: return "?";
    }
}

static inline long long max_ll(long long a, long long b) {
    if (a > b) return a;
    return b;
}

static inline long long min_ll(long long a, long long b) {
    if (a < b) return a;
    return b;
}

/*
 * Write the worker tags in time order into out, for example "H M L".
 * With use_finish the order is by finish time, otherwise by the time each
 * worker first ran. out must hold 2 * count characters.
 * The sort is a plain selection sort: there are only three workers.
 */
static inline void order_by_time(const struct worker *workers, int count, bool use_finish,
                                 char *out) {
    int order[16];
    if (count > 16) count = 16;
    for (int i = 0; i < count; i++) order[i] = i;

    for (int i = 0; i + 1 < count; i++) {
        for (int j = i + 1; j < count; j++) {
            long long ta = workers[order[i]].first_run_ns;
            long long tb = workers[order[j]].first_run_ns;
            if (use_finish) {
                ta = workers[order[i]].finish_ns;
                tb = workers[order[j]].finish_ns;
            }
            if (tb < ta) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    int pos = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0) out[pos++] = ' ';
        out[pos++] = workers[order[i]].tag;
    }
    out[pos] = '\0';
}

/* Did the workers finish in exactly this order, e.g. "H M L"? */
static inline bool finished_in_order(const struct worker *workers, int count,
                                     const char *expected) {
    char order[32];
    order_by_time(workers, count, true, order);
    return strcmp(order, expected) == 0;
}

static inline int max_pieces(const struct worker *workers, int count) {
    int m = 0;
    for (int i = 0; i < count; i++) {
        if (workers[i].npieces > m) m = workers[i].npieces;
    }
    return m;
}

/*
 * Did the workers take turns? True if some worker started a piece between
 * the first run and the finish of another worker. Pauses caused by tasks that
 * are not ours (kernel threads, the fair server, RT throttling) do not count.
 */
static inline bool interleaved(const struct worker *workers, int count) {
    for (int a = 0; a < count; a++) {
        for (int b = 0; b < count; b++) {
            if (a == b) continue;
            for (int i = 0; i < workers[b].npieces; i++) {
                long long start = workers[b].pieces[i].start_ns;
                if (start > workers[a].first_run_ns && start < workers[a].finish_ns) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Did the workers run at the same time on different CPUs? */
static inline bool parallel(const struct worker *workers, int count) {
    for (int i = 0; i < count; i++) {
        if (workers[i].got_cpu != workers[0].got_cpu) return true;
    }
    return false;
}

/* "CPU 3", or a note that the threads are not pinned. */
static inline const char *cpu_text(int cpu) {
    static char buf[32];
    if (cpu < 0) return "any CPU (not pinned)";
    snprintf(buf, sizeof(buf), "CPU %d", cpu);
    return buf;
}

/* How long did worker w run inside the time slice [from, to)? */
static inline long long run_time_in(const struct worker *w, long long from, long long to) {
    long long run = 0;
    for (int i = 0; i < w->npieces; i++) {
        long long start = max_ll(from, w->pieces[i].start_ns);
        long long end = min_ll(to, w->pieces[i].end_ns);
        if (end > start) run += end - start;
    }
    return run;
}

/*
 * Text strip: each column is a slice of time, the letter is the worker that
 * ran in that slice, '*' means several of ours ran at the same time (only
 * possible on different CPUs), '.' means none of ours ran.
 */
static inline void print_strip(const struct worker *workers, int count) {
    long long end = 0;
    for (int i = 0; i < count; i++) end = max_ll(end, workers[i].finish_ns);
    long long width = max_ll(1, end / STRIP_COLS + 1);

    char strip[STRIP_COLS + 1];
    for (int c = 0; c < STRIP_COLS; c++) {
        long long from = c * width;
        long long to = from + width;
        long long best = 0;
        int busy = 0;
        strip[c] = '.';
        for (int i = 0; i < count; i++) {
            long long run = run_time_in(&workers[i], from, to);
            if (run * 2 > width) busy++; /* ran for most of the slice */
            if (run > best && run * 4 > width) {
                best = run;
                strip[c] = workers[i].tag;
            }
        }
        if (busy > 1) strip[c] = '*';
    }
    strip[STRIP_COLS] = '\0';
    printf("\ntimeline, one column = %.1f ms:\n  |%s|\n", width / 1e6, strip);
}

static inline void report(const struct worker *workers, int count) {
    printf("\n%-8s %-13s %-13s %4s %9s %9s %6s\n", "worker", "requested", "obtained", "cpu",
           "first ms", "done ms", "pieces");
    bool mismatch = false;
    for (int i = 0; i < count; i++) {
        const struct worker *w = &workers[i];
        char req[32];
        char got[32];
        snprintf(req, sizeof(req), "%s %d", short_policy(w->policy), w->priority);
        snprintf(got, sizeof(got), "%s %d", short_policy(w->got_policy), w->got_priority);
        if (w->policy != w->got_policy || w->priority != w->got_priority) mismatch = true;
        printf("%c %-6s %-13s %-13s %4d %9.1f %9.1f %6d\n", w->tag, w->name, req, got,
               w->got_cpu, w->first_run_ns / 1e6, w->finish_ns / 1e6, w->npieces);
    }
    if (mismatch) printf("WARNING: some worker did not get the policy it asked for\n");

    print_strip(workers, count);

    char order[32];
    order_by_time(workers, count, false, order);
    printf("start order:  %s\n", order);
    order_by_time(workers, count, true, order);
    printf("finish order: %s\n", order);
    printf("most pieces for one worker: %d\n", max_pieces(workers, count));
    if (interleaved(workers, count)) {
        printf("workers interleaved: yes\n");
    } else {
        printf("workers interleaved: no\n");
    }
    if (parallel(workers, count)) {
        printf("NOTE: workers ran on different CPUs, in parallel. The scheduling policy\n"
               "      had nothing to decide. Pin them to one CPU to see it at work.\n");
    }
}

#endif /* TIMELINE_H */
