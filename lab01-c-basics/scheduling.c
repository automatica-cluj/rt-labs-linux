/*
 * scheduling.c - which scheduling policy am I, and may I change it?
 *
 * The program:
 *   1. prints the priority range of SCHED_OTHER, SCHED_FIFO and SCHED_RR
 *   2. prints this thread's policy and the RLIMIT_RTPRIO limit
 *   3. tries to switch to SCHED_FIFO at the priority given on the command line
 *   4. switches back to SCHED_OTHER
 *
 * Usage:  ./scheduling [priority]      (default 50)
 *
 * Nothing here needs sudo. A normal user may use real-time priorities up to
 * RLIMIT_RTPRIO ('ulimit -r'). On the lab machine that limit is 80, set by
 * the administrator in /etc/security/limits.d/. Ask for more: EPERM.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

static const char *policy_name(int policy) {
    switch (policy) {
        case SCHED_OTHER: return "SCHED_OTHER";
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_RR: return "SCHED_RR";
        default: return "unknown";
    }
}

/* Print the policy and priority the kernel reports for this thread. */
static void show(const char *when) {
    int policy = 0;
    struct sched_param sp;
    pthread_getschedparam(pthread_self(), &policy, &sp);
    printf("   %-8s %s, priority %d\n", when, policy_name(policy), sp.sched_priority);
}

/*
 * Returns 0 on success or the error number. pthread functions do not set
 * errno, so perror() would print a wrong message here.
 */
static int set_policy(int policy, int priority) {
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = priority;
    return pthread_setschedparam(pthread_self(), policy, &sp);
}

int main(int argc, char **argv) {
    int priority = 50;
    if (argc > 1) priority = atoi(argv[1]);

    const int policies[] = {SCHED_OTHER, SCHED_FIFO, SCHED_RR};
    printf("1. priority ranges\n");
    for (int i = 0; i < 3; i++) {
        printf("   %-12s %2d .. %2d\n", policy_name(policies[i]),
               sched_get_priority_min(policies[i]), sched_get_priority_max(policies[i]));
    }

    struct rlimit rl;
    getrlimit(RLIMIT_RTPRIO, &rl);
    printf("\n2. RLIMIT_RTPRIO: soft %ld, hard %ld  (same as 'ulimit -r')\n", (long)rl.rlim_cur,
           (long)rl.rlim_max);
    show("now:");

    printf("\n3. switch to SCHED_FIFO %d\n", priority);
    int rc = set_policy(SCHED_FIFO, priority);
    if (rc != 0) {
        printf("   failed: %s\n", strerror(rc));
        if (rc == EPERM) {
            printf("   priority %d is above your limit of %ld\n", priority, (long)rl.rlim_cur);
        } else if (rc == EINVAL) {
            printf("   SCHED_FIFO priorities are 1..99\n");
        }
        return 1;
    }
    show("after:");

    /* A SCHED_FIFO thread runs until it blocks or something of higher
     * priority wants the CPU. A bug such as while(1){} would starve every
     * normal process on that CPU. The kernel's RT throttling (see
     * /proc/sys/kernel/sched_rt_runtime_us) is the safety net. */

    printf("\n4. back to SCHED_OTHER\n");
    rc = set_policy(SCHED_OTHER, 0);
    if (rc != 0) {
        printf("   failed: %s\n", strerror(rc));
        return 1;
    }
    show("after:");
    return 0;
}
