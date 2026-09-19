/*
 * control_app.c - three periodic tasks under Rate Monotonic Scheduling
 *
 * A small control system, all three threads pinned to the same CPU:
 *
 *   task        period  computation  role
 *   sensor        5 ms       800 us  reads a (simulated) sensor value
 *   controller   10 ms      2000 us  computes an output from the sensor value
 *   logger       20 ms      3000 us  takes a consistent snapshot of both
 *
 * Rate Monotonic: the shorter the period, the higher the priority. With
 * priority P on the command line the tasks get P, P-10 and P-20.
 *
 * Before running, the program does the schedulability analysis:
 *   - total utilisation U against the Liu & Layland bound n(2^(1/n) - 1)
 *   - exact response-time analysis R = C + sum over higher tasks ceil(R/T)*C
 * and afterwards compares the predicted worst case with what it measured.
 *
 * The tasks share data through a mutex with priority inheritance, the right
 * default for any lock shared by real-time threads of different priorities
 * (see lab 5).
 *
 * Usage:  ./control_app [priority] [seconds] [cpu] [load_percent] [reverse]
 *   defaults: 80 5 <per-user cpu> 100 0
 *   priority      0 = all SCHED_OTHER, otherwise 21..80 (three levels needed)
 *   cpu          -1 = do not pin (the tasks then run in parallel)
 *   load_percent  scales every computation time: 150 = 1.5 x the table above
 *   reverse       1 = give the LONGEST period the highest priority (not RMS)
 *
 * Ctrl+C stops early; statistics cover the jobs that actually ran.
 */

#include "rt.h"

#include <math.h>
#include <signal.h>
#include <stdbool.h>

#define NUM_TASKS 3

#define SENSOR 0
#define CONTROLLER 1
#define LOGGER 2

/* Set by the Ctrl+C handler, read by the task loops. sig_atomic_t is the one
 * type a signal handler may safely write; volatile makes the loops re-read it
 * every time instead of keeping a copy in a register. */
static volatile sig_atomic_t stop = 0;

static void on_signal(int signum) {
    (void)signum;
    stop = 1;
}

/* Data flowing sensor -> controller -> logger, protected by plant_lock. */
static pthread_mutex_t plant_lock;
static int sensor_value = 0;
static int control_output = 0;
static long sensor_updates = 0;

struct task {
    /* set by main before the thread starts */
    const char *name;
    int kind; /* SENSOR, CONTROLLER or LOGGER */
    long period_us;
    long wcet_us;
    int priority;
    int cpu;
    struct timespec first_release;
    struct timespec end;

    /* results, written only by the task's own thread */
    struct rt_stats latency;
    struct rt_stats response;
    long jobs;
    long misses;
    long skipped;
    /* logger only: last consistent snapshot */
    int seen_sensor;
    int seen_output;
};

/* One job of each task. The rule for the shared data: hold the lock only to
 * copy a value in or out, and do the slow work (rt_busy_ns) unlocked. While a
 * task holds the lock a higher-priority task may have to wait for it, so the
 * length of the critical section adds directly to that task's response time. */

static void sensor_job(struct task *t) {
    rt_busy_ns(t->wcet_us * RT_NS_PER_US); /* reading the sensor */

    pthread_mutex_lock(&plant_lock);
    sensor_value = (sensor_value + 1) % 1000;
    sensor_updates++;
    pthread_mutex_unlock(&plant_lock);
}

static void controller_job(struct task *t) {
    pthread_mutex_lock(&plant_lock);
    int input = sensor_value;
    pthread_mutex_unlock(&plant_lock);

    rt_busy_ns(t->wcet_us * RT_NS_PER_US); /* the control algorithm */

    pthread_mutex_lock(&plant_lock);
    control_output = 2 * input;
    pthread_mutex_unlock(&plant_lock);
}

static void logger_job(struct task *t) {
    pthread_mutex_lock(&plant_lock);
    t->seen_sensor = sensor_value;
    t->seen_output = control_output;
    pthread_mutex_unlock(&plant_lock);

    rt_busy_ns(t->wcet_us * RT_NS_PER_US); /* formatting, writing to storage... */
}

/* The thread of one task: the periodic loop, with the same structure and
 * overrun policy as periodic_task.c. */
static void *run_task(void *arg) {
    struct task *t = arg;
    long long period_ns = t->period_us * RT_NS_PER_US;
    struct timespec release = t->first_release;

    while (!stop && rt_diff_ns(release, t->end) < 0) {
        /* 1. Wait for the release (absolute time: no drift). */
        rt_sleep_until(release);

        /* 2. Release latency: how late did we start? */
        struct timespec woke = rt_now();
        rt_stats_add(&t->latency, rt_diff_ns(woke, release));

        /* 3. The job. */
        if (t->kind == SENSOR) sensor_job(t);
        if (t->kind == CONTROLLER) controller_job(t);
        if (t->kind == LOGGER) logger_job(t);

        /* 4. Response time and deadline check (deadline = next release). */
        struct timespec done = rt_now();
        long long response = rt_diff_ns(done, release);
        rt_stats_add(&t->response, response);
        if (response > period_ns) t->misses++;
        t->jobs++;

        /* 5. Next release; skip the ones that are already in the past. */
        release = rt_add_ns(release, period_ns);
        long long behind = rt_diff_ns(done, release);
        if (behind > 0) {
            long long lost = behind / period_ns + 1;
            t->skipped += lost;
            release = rt_add_ns(release, lost * period_ns);
        }
    }
    return NULL;
}

/*
 * Exact response-time analysis for fixed priorities on one CPU. The worst
 * case for task i is its own computation plus every job of every
 * higher-priority task that can be released while it is still running:
 *     R = C_i + sum over higher-priority j of ceil(R / T_j) * C_j
 * R appears on both sides, so start with R = C_i and repeat until it stops
 * changing. Returns R in us, or -1 if R grows beyond the period (deadline).
 */
static long response_time_bound(const struct task tasks[], int n, int i) {
    long r = tasks[i].wcet_us;
    for (int iteration = 0; iteration < 1000; iteration++) {
        long next = tasks[i].wcet_us;
        for (int j = 0; j < n; j++) {
            if (j != i && tasks[j].priority > tasks[i].priority) {
                long releases = (long)ceil((double)r / tasks[j].period_us);
                next += releases * tasks[j].wcet_us;
            }
        }
        if (next > tasks[i].period_us) return -1;
        if (next == r) return r;
        r = next;
    }
    return -1;
}

static void init_task(struct task *t, const char *name, int kind, long period_us, long wcet_us) {
    memset(t, 0, sizeof(*t));
    t->name = name;
    t->kind = kind;
    t->period_us = period_us;
    t->wcet_us = wcet_us;
    rt_stats_init(&t->latency);
    rt_stats_init(&t->response);
}

/* Schedulability analysis, printed before anything runs. */
static void print_analysis(const struct task tasks[], int priority, int cpu, bool reverse) {
    double u = 0;
    for (int i = 0; i < NUM_TASKS; i++) u += (double)tasks[i].wcet_us / tasks[i].period_us;
    double bound = NUM_TASKS * (pow(2.0, 1.0 / NUM_TASKS) - 1.0);

    printf("%-10s %8s %8s %6s %8s %12s\n", "task", "period", "wcet", "U", "priority", "R bound");
    for (int i = 0; i < NUM_TASKS; i++) {
        long rb = response_time_bound(tasks, NUM_TASKS, i);
        char text[32];
        if (rb < 0) {
            snprintf(text, sizeof(text), "> period");
        } else {
            snprintf(text, sizeof(text), "%ld us", rb);
        }
        printf("%-10s %5ld ms %5ld us %5.1f%% %8d %12s\n", tasks[i].name,
               tasks[i].period_us / 1000, tasks[i].wcet_us,
               100.0 * tasks[i].wcet_us / tasks[i].period_us, tasks[i].priority, text);
    }

    const char *verdict;
    if (u <= bound) {
        verdict = "schedulable by the bound";
    } else if (u <= 1.0) {
        verdict = "bound inconclusive, see R bound column";
    } else {
        verdict = "overloaded";
    }
    printf("total utilisation %.1f %%, Liu & Layland bound for %d tasks %.1f %%: %s\n", 100 * u,
           NUM_TASKS, 100 * bound, verdict);

    if (priority == 0) printf("priority 0: all tasks SCHED_OTHER, the analysis does not apply\n");
    if (cpu < 0) printf("cpu -1: tasks not pinned, the analysis (one CPU) does not apply\n");
    if (reverse) printf("reverse: priorities are NOT rate monotonic\n");
}

static long long max_us(const struct rt_stats *s) {
    if (s->n == 0) return 0;
    return s->max / RT_NS_PER_US;
}

int main(int argc, char **argv) {
    int priority = (int)rt_arg_long(argc, argv, 1, 80, 0, 99);
    int seconds = (int)rt_arg_long(argc, argv, 2, 5, 1, 3600);
    int cpu = (int)rt_arg_long(argc, argv, 3, rt_default_cpu(), -1, CPU_SETSIZE - 1);
    long load = rt_arg_long(argc, argv, 4, 100, 1, 1000);
    bool reverse = rt_arg_long(argc, argv, 5, 0, 0, 1) == 1;

    if (priority != 0 && priority < 21) {
        fprintf(stderr, "priority must be 0 or at least 21 (the tasks use P, P-10, P-20)\n");
        return 2;
    }

    /* The task set, shortest period first. */
    struct task tasks[NUM_TASKS];
    init_task(&tasks[0], "sensor", SENSOR, 5000, 800 * load / 100);
    init_task(&tasks[1], "controller", CONTROLLER, 10000, 2000 * load / 100);
    init_task(&tasks[2], "logger", LOGGER, 20000, 3000 * load / 100);

    /* Rate Monotonic: level 0 (highest priority) goes to the shortest period.
     * 'reverse' turns the order around to show what goes wrong. */
    for (int i = 0; i < NUM_TASKS; i++) {
        int level = i;
        if (reverse) level = NUM_TASKS - 1 - i;
        if (priority == 0) {
            tasks[i].priority = 0;
        } else {
            tasks[i].priority = priority - 10 * level;
        }
        tasks[i].cpu = cpu;
    }

    print_analysis(tasks, priority, cpu, reverse);

    /* Ctrl+C and 'kill' set the stop flag instead of killing the process. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (rt_lock_memory() != 0) return 1;

    /* A lock shared by threads of different priorities: priority inheritance. */
    if (rt_mutex_init(&plant_lock, 1) != 0) return 1;

    /* Common first release (a "critical instant": all tasks released together,
     * the worst case the analysis assumes). */
    struct timespec start = rt_add_ns(rt_now(), 50 * RT_NS_PER_MS);
    struct timespec end = rt_add_ns(start, seconds * RT_NS_PER_SEC);

    pthread_t threads[NUM_TASKS];
    int created = 0;
    for (int i = 0; i < NUM_TASKS; i++) {
        tasks[i].first_release = start;
        tasks[i].end = end;
        if (rt_start_thread(&threads[i], run_task, &tasks[i], tasks[i].priority, tasks[i].cpu) != 0) {
            stop = 1; /* no real-time priority: stop the threads already started */
            break;
        }
        created++;
    }
    for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
    if (created != NUM_TASKS) return 1;

    /* Results. */
    printf("\nrunning on CPU %d for %d s%s\n", cpu, seconds,
           stop ? " (stopped early by signal)" : "");
    printf("%-10s %6s %7s %7s %13s %13s %13s\n", "task", "jobs", "misses", "skipped",
           "max latency", "avg response", "max response");
    long total_misses = 0;
    for (int i = 0; i < NUM_TASKS; i++) {
        struct task *t = &tasks[i];
        total_misses += t->misses;
        printf("%-10s %6ld %7ld %7ld %10lld us %10lld us %10lld us\n", t->name, t->jobs,
               t->misses, t->skipped, max_us(&t->latency),
               rt_stats_avg(&t->response) / RT_NS_PER_US, max_us(&t->response));
    }
    printf("logger's last snapshot: sensor %d, output %d (%ld sensor updates)\n",
           tasks[LOGGER].seen_sensor, tasks[LOGGER].seen_output, sensor_updates);
    if (total_misses == 0) {
        printf("RESULT: all deadlines met\n");
    } else {
        printf("RESULT: %ld deadline misses\n", total_misses);
    }
    return 0;
}
