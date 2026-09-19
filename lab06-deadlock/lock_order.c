/*
 * lock_order.c - preventing the deadlock of deadlock.c
 *
 * Same two tasks and the same two mutexes. Task A always takes m1 then m2.
 * Task B would naturally take m2 first. The mode decides how B does it:
 *
 *   ordered  B follows the global order m1 -> m2. No cycle is possible
 *            (breaks the "circular wait" condition).
 *   trylock  B takes m2, then *tries* m1. If m1 is busy, B releases m2,
 *            waits a little and starts over (breaks "hold and wait").
 *   address  B calls lock_both(m2, m1), which always locks the mutex with
 *            the lower address first. The order written does not matter:
 *            the rule gives every thread the same global order.
 *   wrong    B takes m2 then m1 with plain locks: the deadlock comes back and
 *            the watchdog reports it.
 *
 * Usage:  ./lock_order [mode] [priority]
 *   mode      ordered (default) | trylock | address | wrong
 *   priority  0 = SCHED_OTHER (default), 1..80 = SCHED_FIFO for both tasks
 *
 * Timeline (times from program start):
 *     0 ms  A locks m1, works 100 ms
 *    50 ms  B starts and needs both mutexes
 *   100 ms  A locks m2, works 50 ms
 *   150 ms  A releases both; B can now get both and works 50 ms
 */

#include "rt.h" /* first: defines _GNU_SOURCE before any system header */

#include <stdatomic.h>
#include <stdint.h>

#define HOLD_MS 100      /* A's work holding only m1 */
#define WORK_MS 50       /* work holding both */
#define B_START_MS 50
#define BACKOFF_MS 5     /* trylock mode: pause before retrying */
#define WATCHDOG_MS 3000

enum mode { MODE_ORDERED, MODE_TRYLOCK, MODE_ADDRESS, MODE_WRONG };

static struct timespec g_start;
static enum mode g_mode = MODE_ORDERED;

/* Both mutexes live in one array, so m1 is guaranteed to have the lower
 * address. That makes "lower address first" the same order as "m1 then m2",
 * which is the order task A uses. */
static pthread_mutex_t g_mutexes[2];
#define g_m1 g_mutexes[0] /* lock order: 1 */
#define g_m2 g_mutexes[1] /* lock order: 2 */
static int g_resource1 = 0;
static int g_resource2 = 0;

struct result {
    double started_ms;
    double got_both_ms;
    double finished_ms;
    int retries;
    atomic_bool done; /* read by the watchdog in main */
};

static struct result g_a;
static struct result g_b;

static void log_event(char task, const char *what) {
    printf("[%6.1f ms] task %c: %s\n", rt_elapsed_ms(g_start), task, what);
    fflush(stdout);
}

/* pthread calls return the error number; they do not set errno. */
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

/* Returns 1 if we got the mutex, 0 if it is busy. Never blocks. */
static int try_lock(pthread_mutex_t *m) {
    int rc = pthread_mutex_trylock(m);
    if (rc == 0) return 1;
    if (rc == EBUSY) return 0;
    die("pthread_mutex_trylock", rc);
    return 0;
}

/*
 * Lock two mutexes in a fixed global order: lower address first. Whatever
 * order the caller names them in, every thread ends up locking them in the
 * same order, so a cycle cannot form.
 */
static void lock_both(pthread_mutex_t *x, pthread_mutex_t *y) {
    if ((uintptr_t)x < (uintptr_t)y) {
        lock_or_die(x);
        lock_or_die(y);
    } else {
        lock_or_die(y);
        lock_or_die(x);
    }
}

static void critical_section(char task, int add) {
    g_resource1 += add;
    g_resource2 += add;
    log_event(task, "has m1 and m2, working");
    rt_sleep_ms(WORK_MS);
}

static void *task_a(void *arg) {
    (void)arg;
    g_a.started_ms = rt_elapsed_ms(g_start);
    lock_or_die(&g_m1);
    log_event('A', "got m1");
    rt_sleep_ms(HOLD_MS);
    log_event('A', "locking m2");
    lock_or_die(&g_m2);
    g_a.got_both_ms = rt_elapsed_ms(g_start);
    critical_section('A', 1);
    g_a.finished_ms = rt_elapsed_ms(g_start);
    log_event('A', "releasing both");
    unlock_or_die(&g_m2);
    unlock_or_die(&g_m1);
    atomic_store(&g_a.done, 1);
    return NULL;
}

/* Each function below gets B both mutexes in a different way. */

static void b_lock_ordered(void) {
    log_event('B', "locking m1 then m2 (global order)");
    lock_or_die(&g_m1);
    lock_or_die(&g_m2);
}

static void b_lock_trylock(void) {
    log_event('B', "locking m2, then trying m1");
    for (;;) {
        lock_or_die(&g_m2);
        if (try_lock(&g_m1)) return;
        unlock_or_die(&g_m2); /* do not hold m2 while waiting for m1 */
        g_b.retries++;
        rt_sleep_ms(BACKOFF_MS);
    }
}

static void b_lock_address(void) {
    log_event('B', "lock_both(m2, m1): lower address first");
    lock_both(&g_m2, &g_m1);
}

static void b_lock_wrong(void) {
    log_event('B', "locking m2 then m1 (WRONG order)");
    lock_or_die(&g_m2);
    log_event('B', "got m2, waiting for m1");
    lock_or_die(&g_m1);
}

static void *task_b(void *arg) {
    (void)arg;
    rt_sleep_ms(B_START_MS);
    g_b.started_ms = rt_elapsed_ms(g_start);

    switch (g_mode) {
        case MODE_ORDERED: b_lock_ordered(); break;
        case MODE_TRYLOCK: b_lock_trylock(); break;
        case MODE_ADDRESS: b_lock_address(); break;
        case MODE_WRONG: b_lock_wrong(); break;
    }
    g_b.got_both_ms = rt_elapsed_ms(g_start);
    critical_section('B', 10);
    unlock_or_die(&g_m1);
    unlock_or_die(&g_m2);

    g_b.finished_ms = rt_elapsed_ms(g_start);
    log_event('B', "released both");
    atomic_store(&g_b.done, 1);
    return NULL;
}

static void report(char name, const struct result *r) {
    printf("  task %c: started %5.1f ms, held both from %5.1f ms, finished %5.1f ms, "
           "run time %5.1f ms",
           name, r->started_ms, r->got_both_ms, r->finished_ms, r->finished_ms - r->started_ms);
    if (name == 'B' && g_mode == MODE_TRYLOCK) printf(", %d retries", r->retries);
    printf("\n");
}

int main(int argc, char **argv) {
    const char *mode = "ordered";
    if (argc > 1) mode = argv[1];

    if (strcmp(mode, "ordered") == 0) {
        g_mode = MODE_ORDERED;
    } else if (strcmp(mode, "trylock") == 0) {
        g_mode = MODE_TRYLOCK;
    } else if (strcmp(mode, "address") == 0) {
        g_mode = MODE_ADDRESS;
    } else if (strcmp(mode, "wrong") == 0) {
        g_mode = MODE_WRONG;
    } else {
        fprintf(stderr, "mode must be ordered, trylock, address or wrong\n");
        return 2;
    }
    int priority = (int)rt_arg_long(argc, argv, 2, 0, 0, 99);
    printf("mode: %s, priority: %d\n\n", mode, priority);

    if (rt_mutex_init(&g_m1, 0) != 0) return 1;
    if (rt_mutex_init(&g_m2, 0) != 0) return 1;

    g_start = rt_now();
    pthread_t ta, tb;
    if (rt_start_thread(&ta, task_a, NULL, priority, -1) != 0) return 1;
    if (rt_start_thread(&tb, task_b, NULL, priority, -1) != 0) return 1;

    /* Watchdog: poll instead of pthread_join, because a deadlocked thread
     * never returns and join would hang with it. */
    while (rt_elapsed_ms(g_start) < WATCHDOG_MS) {
        if (atomic_load(&g_a.done) && atomic_load(&g_b.done)) break;
        rt_sleep_ms(10);
    }
    if (!atomic_load(&g_a.done) || !atomic_load(&g_b.done)) {
        printf("\nDEADLOCK: both threads stuck after %.0f ms (watchdog)\n",
               rt_elapsed_ms(g_start));
        fflush(stdout);
        _exit(3); /* stuck threads cannot be joined: end the process now */
    }
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    printf("\nresult\n");
    report('A', &g_a);
    report('B', &g_b);
    printf("  total %.1f ms, resource1 = %d, resource2 = %d (expected 11 and 11)\n",
           rt_elapsed_ms(g_start), g_resource1, g_resource2);
    printf("  completed, no deadlock\n");
    return 0;
}
