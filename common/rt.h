/*
 * rt.h - the real-time building blocks shared by all labs
 *
 * Lab 0 (hello_rt.c) spells every step out inline. From lab 2 on the same
 * steps live here so each lab can focus on its own topic. Read this file
 * once; it is short and every function is one idea:
 *
 *   rt_lock_memory()      mlockall + no malloc trimming: no page faults later
 *   rt_set_self_sched()   switch the calling thread to SCHED_FIFO or back
 *   rt_start_thread()     create a thread that is real-time from its first line
 *   rt_pin_self_to_cpu()  keep the calling thread on one CPU
 *   rt_sleep_until()      absolute clock_nanosleep, the basis of periodic tasks
 *   rt_mutex_init()       pthread mutex with or without priority inheritance
 *   rt_stats_*            min / avg / max in microseconds
 *   rt_thread_cpu_ns()    CPU time this thread has used
 *   rt_effective_priority()  the priority the scheduler is using right now
 *
 * Include this file FIRST in every program. It defines _GNU_SOURCE, which
 * must come before any system header and unlocks the Linux-specific calls
 * used here (CPU pinning, sched_getcpu).
 *
 * All functions are 'static inline', so there is nothing to link: including
 * the header is enough.
 */

#ifndef RT_H
#define RT_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RT_NS_PER_SEC 1000000000LL
#define RT_NS_PER_MS 1000000LL
#define RT_NS_PER_US 1000LL

/* ---------------------------------------------------------------- time ---- */

/* t + ns, as a new timespec. ns may be larger than one second. */
static inline struct timespec rt_add_ns(struct timespec t, long long ns) {
    ns += t.tv_nsec;
    t.tv_sec += ns / RT_NS_PER_SEC;
    t.tv_nsec = ns % RT_NS_PER_SEC;
    return t;
}

/* (a - b) in nanoseconds. */
static inline long long rt_diff_ns(struct timespec a, struct timespec b) {
    return (long long)(a.tv_sec - b.tv_sec) * RT_NS_PER_SEC + (a.tv_nsec - b.tv_nsec);
}

/* The current time on the clock that never jumps. */
static inline struct timespec rt_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t;
}

static inline long long rt_elapsed_ns(struct timespec since) {
    return rt_diff_ns(rt_now(), since);
}

static inline double rt_elapsed_ms(struct timespec since) {
    return rt_elapsed_ns(since) / 1e6;
}

/*
 * CPU time used by the calling thread, as opposed to wall-clock time. Work
 * measured in CPU time really does take longer when the thread is preempted.
 */
static inline long long rt_thread_cpu_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (long long)t.tv_sec * RT_NS_PER_SEC + t.tv_nsec;
}

/*
 * Sleep until an absolute CLOCK_MONOTONIC time. The deadline does not move
 * if a signal wakes us early, so we simply sleep again.
 * Note: clock_nanosleep returns the error number, it does not set errno.
 */
static inline void rt_sleep_until(struct timespec deadline) {
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    } while (rc == EINTR);
}

static inline void rt_sleep_ms(long ms) {
    rt_sleep_until(rt_add_ns(rt_now(), ms * RT_NS_PER_MS));
}

/* Burn CPU for about ns nanoseconds. Stands in for "real computation". */
static inline void rt_busy_ns(long long ns) {
    struct timespec start = rt_now();
    volatile unsigned long x = 0; /* volatile: the optimiser must keep the loop */
    while (rt_elapsed_ns(start) < ns) {
        for (int i = 0; i < 1000; i++) x += i;
    }
}

/* ---------------------------------------------------------- scheduling ---- */

static inline const char *rt_policy_name(int policy) {
    switch (policy) {
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_RR: return "SCHED_RR";
        case SCHED_OTHER: return "SCHED_OTHER";
        default: return "other";
    }
}

/* Print why a scheduling call failed, with a hint for the usual cause. */
static inline void rt_explain_sched_error(const char *what, int rc, int priority) {
    fprintf(stderr, "%s(priority %d): %s\n", what, priority, strerror(rc));
    if (rc == EPERM) {
        fprintf(stderr, "hint: 'ulimit -r' shows the highest real-time priority you may use\n");
    }
}

/*
 * Switch the calling thread to `policy` at `priority`.
 * Priority 0 always means SCHED_OTHER. Returns 0 or the error number.
 */
static inline int rt_set_self_policy(int policy, int priority) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    if (priority == 0) policy = SCHED_OTHER;
    sp.sched_priority = priority;
    int rc = pthread_setschedparam(pthread_self(), policy, &sp);
    if (rc != 0) rt_explain_sched_error("pthread_setschedparam", rc, priority);
    return rc;
}

/* The common case: SCHED_FIFO at `priority`, or SCHED_OTHER for priority 0. */
static inline int rt_set_self_sched(int priority) {
    return rt_set_self_policy(SCHED_FIFO, priority);
}

/* Read back what the kernel actually gave us. Never trust what you asked for. */
static inline void rt_print_self_sched(const char *who) {
    int policy = 0;
    struct sched_param sp;
    pthread_getschedparam(pthread_self(), &policy, &sp);
    printf("%s: %s priority %d, CPU %d\n", who, rt_policy_name(policy), sp.sched_priority,
           sched_getcpu());
}

/* The priority this thread asked for (without any inheritance boost). */
static inline int rt_self_priority(void) {
    int policy = 0;
    struct sched_param sp;
    pthread_getschedparam(pthread_self(), &policy, &sp);
    return sp.sched_priority;
}

/*
 * The priority the scheduler is using for this thread right now, including a
 * boost from priority inheritance. pthread_getschedparam() reports only what
 * we asked for ourselves, so read field 18 of /proc/thread-self/stat: for a
 * real-time thread it holds -1 - effective_priority.
 */
static inline int rt_effective_priority(void) {
    FILE *f = fopen("/proc/thread-self/stat", "r");
    if (f == NULL) return -1;
    char line[512];
    char *got = fgets(line, sizeof(line), f);
    fclose(f);
    if (got == NULL) return -1;

    /* Field 2 is the thread name in brackets and may contain spaces, so
     * start after the last ')'. The next token is field 3. */
    char *rest = strrchr(line, ')');
    if (rest == NULL) return -1;
    rest++;

    char *save = NULL; /* strtok_r keeps its state here: safe in any thread */
    char *token = strtok_r(rest, " ", &save);
    for (int field = 3; token != NULL; field++) {
        if (field == 18) return -atoi(token) - 1;
        token = strtok_r(NULL, " ", &save);
    }
    return -1;
}

/* ---------------------------------------------------------------- CPUs ---- */

static inline int rt_pin_self_to_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) fprintf(stderr, "pin to CPU %d: %s\n", cpu, strerror(rc));
    return rc;
}

/*
 * A CPU for single-CPU experiments. On the shared machine twenty students
 * pinning to CPU 0 would measure each other, so the default is derived from
 * the user id: different accounts land on different CPUs.
 */
static inline int rt_default_cpu(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) != 0) return 0;
    int count = CPU_COUNT(&set);
    if (count <= 0) return 0;
    int wanted = (int)(getuid() % (unsigned)count);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &set)) {
            if (wanted == 0) return cpu;
            wanted--;
        }
    }
    return 0;
}

/*
 * Create a thread whose policy, priority and CPU are set BEFORE it runs.
 *   priority 0 -> SCHED_OTHER,  cpu < 0 -> no pinning.
 * PTHREAD_EXPLICIT_SCHED is essential: without it the attributes are ignored
 * and the new thread silently inherits the creator's policy.
 * Returns 0 or the error number (EPERM when the priority is over the limit).
 */
static inline int rt_start_thread_policy(pthread_t *thread, void *(*fn)(void *), void *arg,
                                         int policy, int priority, int cpu) {
    pthread_attr_t attr;
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    if (priority == 0) policy = SCHED_OTHER;
    sp.sched_priority = priority;

    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, policy);
    pthread_attr_setschedparam(&attr, &sp);
    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
    }
    int rc = pthread_create(thread, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) rt_explain_sched_error("pthread_create", rc, priority);
    return rc;
}

/* The common case: a SCHED_FIFO thread (or SCHED_OTHER for priority 0). */
static inline int rt_start_thread(pthread_t *thread, void *(*fn)(void *), void *arg,
                                  int priority, int cpu) {
    return rt_start_thread_policy(thread, fn, arg, SCHED_FIFO, priority, cpu);
}

/* -------------------------------------------------------------- memory ---- */

/*
 * Lock all memory in RAM and stop malloc from giving memory back to the
 * kernel. After this, the time-critical code cannot take a page fault.
 * Returns 0 on success, -1 on failure (with a message).
 */
static inline int rt_lock_memory(void) {
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        perror("mlockall"); /* mlockall does set errno */
        fprintf(stderr, "hint: check 'ulimit -l' (memlock limit)\n");
        return -1;
    }
    mallopt(M_TRIM_THRESHOLD, -1); /* never return freed heap memory */
    mallopt(M_MMAP_MAX, 0);        /* serve large allocations from the heap */
    /* Touch a chunk of stack now so its pages are already present. */
    volatile char stack[64 * 1024];
    for (size_t i = 0; i < sizeof(stack); i += 4096) stack[i] = 0;
    return 0;
}

/* --------------------------------------------------------------- mutex ---- */

/*
 * Initialise a pthread mutex.
 *   inherit = 1  priority inheritance (PTHREAD_PRIO_INHERIT)
 *   inherit = 0  plain mutex (PTHREAD_PRIO_NONE), the default kind
 * Afterwards use the normal calls: pthread_mutex_lock / pthread_mutex_unlock.
 * Returns 0 or the error number.
 */
static inline int rt_mutex_init(pthread_mutex_t *mutex, int inherit) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    int rc = pthread_mutexattr_setprotocol(&attr,
                                           inherit ? PTHREAD_PRIO_INHERIT : PTHREAD_PRIO_NONE);
    if (rc == 0) rc = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc != 0) fprintf(stderr, "rt_mutex_init: %s\n", strerror(rc));
    return rc;
}

/* --------------------------------------------------------------- stats ---- */

struct rt_stats {
    long long min;
    long long max;
    long long sum;
    long long n;
};

static inline void rt_stats_init(struct rt_stats *s) {
    s->min = LLONG_MAX;
    s->max = LLONG_MIN;
    s->sum = 0;
    s->n = 0;
}

static inline void rt_stats_add(struct rt_stats *s, long long ns) {
    if (ns < s->min) s->min = ns;
    if (ns > s->max) s->max = ns;
    s->sum += ns;
    s->n++;
}

static inline long long rt_stats_avg(const struct rt_stats *s) {
    return s->n > 0 ? s->sum / s->n : 0;
}

/* Real-time is about the worst case: read the max, not the avg. */
static inline void rt_stats_print_us(const struct rt_stats *s, const char *label) {
    if (s->n == 0) {
        printf("%s: no samples\n", label);
        return;
    }
    printf("%s over %lld samples: min %lld us, avg %lld us, max %lld us\n", label, s->n,
           s->min / RT_NS_PER_US, rt_stats_avg(s) / RT_NS_PER_US, s->max / RT_NS_PER_US);
}

/* ----------------------------------------------------------- arguments ---- */

/*
 * Positional integer argument argv[index], with a default and a valid range.
 * Exits with a message on garbage such as "abc" or out-of-range values.
 */
static inline long rt_arg_long(int argc, char **argv, int index, long fallback, long lo,
                               long hi) {
    if (argc <= index) return fallback;
    char *end = NULL;
    errno = 0;
    long v = strtol(argv[index], &end, 10);
    if (errno != 0 || end == argv[index] || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "argument %d ('%s') must be an integer in [%ld, %ld]\n", index,
                argv[index], lo, hi);
        exit(2);
    }
    return v;
}

#endif /* RT_H */
