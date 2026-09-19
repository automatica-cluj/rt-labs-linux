/*
 * inversion.c - priority inversion, and priority inheritance as the fix
 *
 * Three SCHED_FIFO threads share one CPU and one mutex:
 *
 *   LOW    priority 20  locks the mutex, then needs CS ms of CPU inside it
 *   HIGH   priority 60  arrives while LOW holds the mutex and blocks on it
 *   MEDIUM priority 40  does not touch the mutex, just burns MEDIUM ms of CPU
 *
 * With a plain mutex MEDIUM preempts LOW, so HIGH waits for MEDIUM too: the
 * blocking time grows with MEDIUM's work and has no bound (unbounded
 * inversion). With PTHREAD_PRIO_INHERIT the kernel lends HIGH's priority to
 * LOW while HIGH waits, MEDIUM cannot preempt LOW, and HIGH waits at most for
 * the rest of LOW's critical section (bounded inversion).
 *
 * Usage:  ./inversion [none|inherit] [cpu] [medium_ms] [cs_ms]
 *   none       plain mutex (PTHREAD_PRIO_NONE)            default
 *   inherit    priority inheritance (PTHREAD_PRIO_INHERIT)
 *   cpu        CPU all three threads share; -1 = no pinning  (default: per user)
 *   medium_ms  CPU time MEDIUM burns                      (default 200)
 *   cs_ms      CPU time LOW needs inside the mutex        (default 50)
 *
 * The sequence is driven by semaphores, not by sleeps, so it is the same on
 * every run: LOW locks -> HIGH requests -> MEDIUM starts.
 */

#include "rt.h" /* must be first: it defines _GNU_SOURCE */

#include <semaphore.h>
#include <stdatomic.h>

#define PRIO_LOW 20
#define PRIO_MEDIUM 40
#define PRIO_HIGH 60
#define HIGH_WORK_MS 5
#define NUM_EVENTS 6

/* Everything the three threads share. main fills it in and reads it back. */
struct scenario {
    pthread_mutex_t mutex; /* the shared resource LOW and HIGH fight over */
    long long medium_ns;   /* CPU time MEDIUM burns */
    long long cs_ns;       /* CPU time LOW needs inside the mutex */

    /*
     * Semaphores put the steps in a fixed order. Sleeps would not: "sleep
     * 10 ms and hope LOW has the lock by now" fails as soon as the machine
     * is loaded, and then the demo shows something different on every run.
     */
    sem_t low_locked;      /* LOW holds the mutex */
    sem_t high_requesting; /* HIGH is about to call pthread_mutex_lock */

    /*
     * Timestamps. Each is written by exactly one thread and read by main
     * only after all threads have been joined, so they need no protection.
     */
    struct timespec t0;
    struct timespec low_lock;
    struct timespec low_unlock;
    struct timespec high_request;
    struct timespec high_acquired;
    struct timespec medium_start;
    struct timespec medium_end;
    int low_cpu;
    int medium_cpu;
    int high_cpu;

    /*
     * HIGH sets this flag, LOW reads it at the same time. A plain int (even
     * a volatile one) is not enough for that: the C standard calls it a data
     * race and the compiler may keep a stale copy. atomic_bool makes every
     * read and write a single, complete step.
     */
    atomic_bool high_waiting;
    int low_effective_prio; /* LOW's priority, boost included, while HIGH waits */
};

/* One line of the timeline printed at the end. */
struct event {
    struct timespec t;
    const char *what;
    int cpu; /* -1: do not print a CPU */
};

/*
 * LOW: takes the mutex first, then needs cs_ns of CPU time inside it.
 *
 * The work is measured in CPU time, not wall-clock time. If MEDIUM preempts
 * LOW, LOW's clock stops, the critical section stays unfinished, and the
 * mutex stays locked. That is exactly how HIGH ends up waiting for MEDIUM.
 */
static void *low_task(void *arg) {
    struct scenario *s = arg;
    s->low_cpu = sched_getcpu();

    pthread_mutex_lock(&s->mutex);
    s->low_lock = rt_now();
    sem_post(&s->low_locked); /* tell main: LOW has the mutex */

    /* ---- critical section: burn cs_ns of CPU ---- */
    long long start = rt_thread_cpu_ns();
    int sampled = 0;
    volatile unsigned long x = 0;
    while (rt_thread_cpu_ns() - start < s->cs_ns) {
        for (int i = 0; i < 1000; i++) x += i;

        /* Once HIGH is blocked on our mutex, look at the priority the
         * scheduler is really using for us: 20, or boosted to 60? */
        if (!sampled && atomic_load(&s->high_waiting)) {
            s->low_effective_prio = rt_effective_priority();
            sampled = 1;
        }
    }
    /* ---- end of critical section ---- */

    s->low_unlock = rt_now();
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

/*
 * HIGH: the important task. It asks for the mutex while LOW holds it, so it
 * blocks. Its blocking time (request -> acquired) is what this lab measures.
 */
static void *high_task(void *arg) {
    struct scenario *s = arg;
    s->high_cpu = sched_getcpu();

    s->high_request = rt_now();
    sem_post(&s->high_requesting); /* tell main: HIGH is about to block */
    atomic_store(&s->high_waiting, 1);

    pthread_mutex_lock(&s->mutex); /* blocks here until LOW unlocks */
    s->high_acquired = rt_now();
    rt_busy_ns(HIGH_WORK_MS * RT_NS_PER_MS); /* short use of the resource */
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

/*
 * MEDIUM: never touches the mutex. It only burns CPU. Its priority is above
 * LOW's, so it preempts LOW whenever LOW runs at its own priority 20, and it
 * cannot preempt LOW when LOW has inherited HIGH's priority 60.
 */
static void *medium_task(void *arg) {
    struct scenario *s = arg;
    s->medium_cpu = sched_getcpu();

    s->medium_start = rt_now();
    long long start = rt_thread_cpu_ns();
    volatile unsigned long x = 0;
    while (rt_thread_cpu_ns() - start < s->medium_ns) {
        for (int i = 0; i < 1000; i++) x += i;
    }
    s->medium_end = rt_now();
    return NULL;
}

/* Time t in milliseconds since t0. */
static double ms_since(struct timespec t, struct timespec t0) {
    return rt_diff_ns(t, t0) / 1e6;
}

/* Length in ms of the overlap of the intervals [a0, a1] and [b0, b1]. */
static double overlap_ms(struct timespec a0, struct timespec a1, struct timespec b0,
                         struct timespec b1) {
    struct timespec start = a0; /* the later of the two starts */
    if (rt_diff_ns(b0, a0) > 0) start = b0;

    struct timespec end = a1; /* the earlier of the two ends */
    if (rt_diff_ns(b1, a1) < 0) end = b1;

    long long d = rt_diff_ns(end, start);
    if (d < 0) return 0.0;
    return d / 1e6;
}

/*
 * Start one real-time thread or stop the program. Carrying on without
 * real-time priorities would print numbers that mean nothing.
 */
static void start_or_die(pthread_t *t, void *(*fn)(void *), struct scenario *s, int prio,
                         int cpu, const char *name) {
    if (rt_start_thread(t, fn, s, prio, cpu) != 0) {
        fprintf(stderr, "could not start %s as SCHED_FIFO %d; this lab needs real-time "
                        "priorities (see lab00 check_env.sh)\n", name, prio);
        exit(1);
    }
}

/* Put the events in time order. Six entries, so a plain bubble sort. */
static void sort_events(struct event events[], int count) {
    for (int i = 0; i + 1 < count; i++) {
        for (int j = 0; j + 1 < count - i; j++) {
            if (rt_diff_ns(events[j + 1].t, events[j].t) < 0) {
                struct event tmp = events[j];
                events[j] = events[j + 1];
                events[j + 1] = tmp;
            }
        }
    }
}

int main(int argc, char **argv) {
    const char *protocol = "none";
    if (argc > 1) protocol = argv[1];

    int inherit = (strcmp(protocol, "inherit") == 0);
    if (!inherit && strcmp(protocol, "none") != 0) {
        fprintf(stderr, "usage: %s [none|inherit] [cpu] [medium_ms] [cs_ms]\n", argv[0]);
        return 2;
    }
    int cpu = (int)rt_arg_long(argc, argv, 2, rt_default_cpu(), -1, CPU_SETSIZE - 1);
    long medium_ms = rt_arg_long(argc, argv, 3, 200, 0, 500);
    long cs_ms = rt_arg_long(argc, argv, 4, 50, 1, 300);

    if (rt_lock_memory() != 0) return 1;

    struct scenario s;
    memset(&s, 0, sizeof(s));
    s.medium_ns = medium_ms * RT_NS_PER_MS;
    s.cs_ns = cs_ms * RT_NS_PER_MS;
    s.low_cpu = -1;
    s.medium_cpu = -1;
    s.high_cpu = -1;
    s.low_effective_prio = -1;
    atomic_init(&s.high_waiting, 0);
    sem_init(&s.low_locked, 0, 0);
    sem_init(&s.high_requesting, 0, 0);

    /* The only line that differs between the two experiments. */
    if (rt_mutex_init(&s.mutex, inherit) != 0) return 1;

    if (inherit) {
        printf("mutex protocol: PTHREAD_PRIO_INHERIT\n");
    } else {
        printf("mutex protocol: PTHREAD_PRIO_NONE\n");
    }
    if (cpu >= 0) {
        printf("all threads pinned to CPU %d\n", cpu);
    } else {
        printf("no pinning: threads may run on different CPUs\n");
    }
    printf("LOW prio %d needs %ld ms inside the mutex, MEDIUM prio %d burns %ld ms, "
           "HIGH prio %d\n\n", PRIO_LOW, cs_ms, PRIO_MEDIUM, medium_ms, PRIO_HIGH);

    /*
     * main stays SCHED_OTHER and only orchestrates. It is not pinned, so it
     * runs on another CPU and does not disturb the three real-time threads.
     * The order is the classic inversion scenario:
     *   1. LOW locks   2. HIGH requests and blocks   3. MEDIUM becomes ready
     */
    pthread_t low, medium, high;
    s.t0 = rt_now();
    start_or_die(&low, low_task, &s, PRIO_LOW, cpu, "LOW");
    sem_wait(&s.low_locked);
    start_or_die(&high, high_task, &s, PRIO_HIGH, cpu, "HIGH");
    sem_wait(&s.high_requesting);
    rt_sleep_ms(1); /* let HIGH actually go to sleep on the mutex */
    start_or_die(&medium, medium_task, &s, PRIO_MEDIUM, cpu, "MEDIUM");

    pthread_join(high, NULL);
    pthread_join(medium, NULL);
    pthread_join(low, NULL);
    sem_destroy(&s.low_locked);
    sem_destroy(&s.high_requesting);
    pthread_mutex_destroy(&s.mutex);

    /*
     * Timeline, printed only now: printf inside the threads would change the
     * very timing we are trying to observe.
     */
    struct event events[NUM_EVENTS] = {
        {s.low_lock, "LOW    locks the mutex", s.low_cpu},
        {s.high_request, "HIGH   requests the mutex and blocks", s.high_cpu},
        {s.medium_start, "MEDIUM starts running", s.medium_cpu},
        {s.medium_end, "MEDIUM finishes", -1},
        {s.low_unlock, "LOW    unlocks", -1},
        {s.high_acquired, "HIGH   gets the mutex", -1},
    };
    sort_events(events, NUM_EVENTS);

    printf("timeline (ms since start)\n");
    for (int i = 0; i < NUM_EVENTS; i++) {
        if (events[i].cpu >= 0) {
            printf("  %7.1f  %-38s (CPU %d)\n", ms_since(events[i].t, s.t0), events[i].what,
                   events[i].cpu);
        } else {
            printf("  %7.1f  %s\n", ms_since(events[i].t, s.t0), events[i].what);
        }
    }
    if (s.low_effective_prio >= 0) {
        printf("\nLOW's effective priority while HIGH waited: %d\n", s.low_effective_prio);
    }

    /* Blocking time: from HIGH's request until HIGH holds the mutex. */
    double blocked = ms_since(s.high_acquired, s.high_request);
    double medium_inside =
        overlap_ms(s.medium_start, s.medium_end, s.high_request, s.high_acquired);
    printf("\nHIGH blocked for %.1f ms (LOW's whole critical section is %ld ms)\n", blocked,
           cs_ms);
    printf("MEDIUM ran for %.1f ms of that time\n", medium_inside);

    /*
     * The bound with priority inheritance is the critical section itself.
     * Allow a little slack for thread start-up and timer resolution.
     */
    double bound = cs_ms * 1.2 + 5.0;
    if (blocked > bound) {
        printf("UNBOUNDED: HIGH waited %.1f ms, longer than the %ld ms critical section, "
               "because MEDIUM ran in between\n", blocked, cs_ms);
    } else {
        printf("BOUNDED: HIGH waited %.1f ms, no longer than the %ld ms critical section\n",
               blocked, cs_ms);
    }
    return 0;
}
