/*
 * deadlock.c - two tasks, two mutexes, opposite lock order
 *
 *   Task A: lock m1, work, then lock m2
 *   Task B: lock m2, work, then lock m1      <- opposite order: circular wait
 *
 * With a timeout (the default) each second lock uses pthread_mutex_clocklock
 * on CLOCK_MONOTONIC. A task that times out treats it as a deadlock: it gives
 * back the mutex it holds, backs off and tries again.
 *
 * With timeout 0 both tasks use a plain pthread_mutex_lock and block forever.
 * A watchdog in main notices that nobody finished and ends the program, so
 * your terminal never hangs.
 *
 * Usage:  ./deadlock [timeout_ms] [jitter_ms] [priority]
 *   timeout_ms  0 = no timeout (real deadlock), default 1000
 *   jitter_ms   random extra delay 0..jitter before the second lock, default 0
 *   priority    0 = SCHED_OTHER (default), 1..80 = SCHED_FIFO for both tasks
 *
 * With jitter 0 the timeline is fixed (times from program start):
 *     0 ms  A locks m1
 *    50 ms  B locks m2
 *   100 ms  A waits for m2       (deadline at 100 + timeout)
 *   150 ms  B waits for m1       (deadline at 150 + timeout)
 *  1100 ms  A times out, releases m1 and backs off
 *  1100 ms  B gets m1 at once (it never times out), works, releases both
 *  1300 ms  A tries again and succeeds
 *
 * What a timeout costs in a real-time system: the task that times out has
 * waited the whole timeout for nothing, and then waits again. That is a
 * large, unpredictable delay (jitter). Detection is a safety net, not a
 * design. lock_order.c shows how to prevent the deadlock instead.
 */

#include "rt.h" /* first: defines _GNU_SOURCE, needed for pthread_mutex_clocklock */

#include <stdatomic.h>

#define MAX_ATTEMPTS 3
#define HOLD_MS 100            /* work done while holding the first mutex */
#define WORK_MS 50             /* work done while holding both */
#define WATCHDOG_EXTRA_MS 2000

static struct timespec g_start;
static long g_timeout_ms = 1000;
static long g_jitter_ms = 0;

static pthread_mutex_t g_m1;
static pthread_mutex_t g_m2;
static int g_resource1 = 0; /* protected by m1 */
static int g_resource2 = 0; /* protected by m2 */

/* Who holds each mutex: 'A', 'B' or '-'. Several threads read and write
 * these, so they are atomic. Printed at the end and by the watchdog. */
static atomic_int g_m1_owner = '-';
static atomic_int g_m2_owner = '-';

struct task {
    char name; /* 'A' or 'B' */
    pthread_mutex_t *first;
    pthread_mutex_t *second;
    atomic_int *first_owner;
    atomic_int *second_owner;
    const char *first_name;
    const char *second_name;
    long start_delay_ms;
    long backoff_ms;
    int add1; /* what the task adds to resource1 */
    int add2; /* what the task adds to resource2 */
    /* results */
    atomic_int attempts;
    int timeouts;
    atomic_bool done;   /* finished its work */
    atomic_bool exited; /* left run_task: done, or gave up */
    double finished_at_ms;
};

static void log_event(const struct task *t, const char *what, const char *mutex_name) {
    printf("[%7.1f ms] task %c: %s%s\n", rt_elapsed_ms(g_start), t->name, what, mutex_name);
    fflush(stdout);
}

/* An error that is not a timeout is a bug in the program, not a deadlock.
 * pthread calls return the error number; they do not set errno. */
static void die(const char *what, int rc) {
    fprintf(stderr, "%s: %s\n", what, strerror(rc));
    abort();
}

static void lock_or_die(pthread_mutex_t *m) {
    int rc = pthread_mutex_lock(m);
    if (rc != 0) die("pthread_mutex_lock", rc);
}

static void unlock_or_die(pthread_mutex_t *m) {
    int rc = pthread_mutex_unlock(m);
    if (rc != 0) die("pthread_mutex_unlock", rc);
}

/*
 * Lock with a deadline. Returns 1 if we got the mutex, 0 on ETIMEDOUT.
 * The deadline is an absolute time on CLOCK_MONOTONIC, the clock that never
 * jumps. (pthread_mutex_timedlock uses the wall clock, which can.)
 */
static int lock_with_timeout(pthread_mutex_t *m, long timeout_ms) {
    if (timeout_ms == 0) {
        lock_or_die(m); /* no timeout: may block forever */
        return 1;
    }
    struct timespec deadline = rt_add_ns(rt_now(), timeout_ms * RT_NS_PER_MS);
    int rc = pthread_mutex_clocklock(m, CLOCK_MONOTONIC, &deadline);
    if (rc == 0) return 1;
    if (rc == ETIMEDOUT) return 0;
    die("pthread_mutex_clocklock", rc);
    return 0;
}

static void *run_task(void *arg) {
    struct task *t = arg;
    unsigned int seed = (unsigned int)rt_now().tv_nsec + (unsigned int)t->name;

    rt_sleep_ms(t->start_delay_ms);

    while (atomic_load(&t->attempts) < MAX_ATTEMPTS) {
        atomic_fetch_add(&t->attempts, 1);

        log_event(t, "locking ", t->first_name);
        lock_or_die(t->first);
        atomic_store(t->first_owner, t->name);
        log_event(t, "got ", t->first_name);

        /* Work while holding the first mutex. rand_r keeps its state in
         * 'seed', so each thread has its own random sequence. */
        long hold = HOLD_MS;
        if (g_jitter_ms > 0) hold += rand_r(&seed) % (g_jitter_ms + 1);
        rt_sleep_ms(hold);

        /* Hold one mutex and wait for another: the "hold and wait" condition. */
        log_event(t, "waiting for ", t->second_name);
        if (!lock_with_timeout(t->second, g_timeout_ms)) {
            /* Recovery: give back what we hold. This breaks "hold and wait"
             * after the fact, so the other task can finish. */
            t->timeouts++;
            log_event(t, "TIMEOUT, assuming deadlock, releasing ", t->first_name);
            atomic_store(t->first_owner, '-');
            unlock_or_die(t->first);
            rt_sleep_ms(t->backoff_ms);
            continue;
        }
        atomic_store(t->second_owner, t->name);
        log_event(t, "got ", t->second_name);

        g_resource1 += t->add1;
        g_resource2 += t->add2;
        rt_sleep_ms(WORK_MS);

        atomic_store(t->second_owner, '-');
        unlock_or_die(t->second);
        atomic_store(t->first_owner, '-');
        unlock_or_die(t->first);
        t->finished_at_ms = rt_elapsed_ms(g_start);
        log_event(t, "done, released both", "");
        atomic_store(&t->done, 1);
        atomic_store(&t->exited, 1);
        return NULL;
    }
    log_event(t, "GAVE UP after max attempts", "");
    atomic_store(&t->exited, 1);
    return NULL;
}

/* The longest the program can legitimately take, plus a margin. */
static long watchdog_limit_ms(void) {
    if (g_timeout_ms == 0) {
        return HOLD_MS + g_jitter_ms + WATCHDOG_EXTRA_MS;
    }
    /* Worst case for one task: every attempt waits hold + jitter + timeout + backoff. */
    long per_attempt = HOLD_MS + g_jitter_ms + g_timeout_ms + 300 + WORK_MS;
    return MAX_ATTEMPTS * per_attempt + WATCHDOG_EXTRA_MS;
}

int main(int argc, char **argv) {
    g_timeout_ms = rt_arg_long(argc, argv, 1, 1000, 0, 60000);
    g_jitter_ms = rt_arg_long(argc, argv, 2, 0, 0, 10000);
    int priority = (int)rt_arg_long(argc, argv, 3, 0, 0, 99);
    long watchdog_ms = watchdog_limit_ms();

    if (g_timeout_ms == 0) {
        printf("timeout: none (plain pthread_mutex_lock), watchdog after %ld ms\n", watchdog_ms);
    } else {
        printf("timeout: %ld ms (pthread_mutex_clocklock, CLOCK_MONOTONIC)\n", g_timeout_ms);
    }
    printf("jitter: %ld ms, priority: %d\n\n", g_jitter_ms, priority);

    if (rt_mutex_init(&g_m1, 0) != 0) return 1;
    if (rt_mutex_init(&g_m2, 0) != 0) return 1;

    struct task a;
    memset(&a, 0, sizeof(a));
    a.name = 'A';
    a.first = &g_m1;
    a.second = &g_m2;
    a.first_owner = &g_m1_owner;
    a.second_owner = &g_m2_owner;
    a.first_name = "m1";
    a.second_name = "m2";
    a.start_delay_ms = 0;
    a.backoff_ms = 200;
    a.add1 = 1;
    a.add2 = 1;

    struct task b;
    memset(&b, 0, sizeof(b));
    b.name = 'B';
    b.first = &g_m2; /* opposite order: this is what closes the cycle */
    b.second = &g_m1;
    b.first_owner = &g_m2_owner;
    b.second_owner = &g_m1_owner;
    b.first_name = "m2";
    b.second_name = "m1";
    b.start_delay_ms = 50;
    b.backoff_ms = 300; /* a different back-off than A avoids livelock */
    b.add1 = 10;
    b.add2 = 10;

    g_start = rt_now();
    pthread_t ta, tb;
    if (rt_start_thread(&ta, run_task, &a, priority, -1) != 0) return 1;
    if (rt_start_thread(&tb, run_task, &b, priority, -1) != 0) return 1;

    /* Watchdog: poll instead of pthread_join, because a deadlocked thread
     * never returns and join would hang with it. */
    while (rt_elapsed_ms(g_start) < watchdog_ms) {
        if (atomic_load(&a.exited) && atomic_load(&b.exited)) break;
        rt_sleep_ms(10);
    }

    if (!atomic_load(&a.done) || !atomic_load(&b.done)) {
        int owner1 = atomic_load(&g_m1_owner);
        int owner2 = atomic_load(&g_m2_owner);
        int stuck = (owner1 != '-' && owner2 != '-');
        if (stuck) {
            printf("\nDEADLOCK: both threads stuck after %.0f ms\n", rt_elapsed_ms(g_start));
        } else {
            printf("\nNOT FINISHED: a task gave up after %.0f ms\n", rt_elapsed_ms(g_start));
        }
        printf("  m1 held by task %c, m2 held by task %c\n", owner1, owner2);
        fflush(stdout);
        /* Stuck threads can never be joined, and a normal exit() could wait
         * on them. _exit ends the process immediately. */
        if (stuck) _exit(3);
        _exit(4);
    }

    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    printf("\nresult\n");
    printf("  task A: %d attempt(s), %d timeout(s), finished at %.0f ms\n",
           atomic_load(&a.attempts), a.timeouts, a.finished_at_ms);
    printf("  task B: %d attempt(s), %d timeout(s), finished at %.0f ms\n",
           atomic_load(&b.attempts), b.timeouts, b.finished_at_ms);
    printf("  resource1 = %d, resource2 = %d (expected 11 and 11)\n", g_resource1, g_resource2);
    if (a.timeouts + b.timeouts > 0) {
        printf("  deadlock detected %d time(s) by timeout and recovered\n",
               a.timeouts + b.timeouts);
    } else {
        printf("  no timeout fired: the tasks did not overlap this time\n");
    }
    return 0;
}
