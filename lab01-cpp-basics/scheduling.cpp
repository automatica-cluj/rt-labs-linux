// scheduling.cpp - which scheduling policy am I, and may I change it?
//
// The program:
//   1. prints the priority range of SCHED_OTHER, SCHED_FIFO and SCHED_RR
//   2. prints this thread's policy and the RLIMIT_RTPRIO limit
//   3. tries to switch to SCHED_FIFO at the priority given on the command line
//   4. switches back to SCHED_OTHER
//
// Usage:  ./scheduling [priority]      (default 50)
//
// Nothing here needs sudo. A normal user may use real-time priorities up to
// RLIMIT_RTPRIO ('ulimit -r'). On the lab machine that limit is 80, set by the
// administrator in /etc/security/limits.d/. Ask for more and you get EPERM.

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include <cerrno>
#include <cstdio>
#include <initializer_list>
#include <cstdlib>
#include <cstring>

namespace {

const char* policy_name(int policy) {
    switch (policy) {
        case SCHED_OTHER: return "SCHED_OTHER";
        case SCHED_FIFO: return "SCHED_FIFO";
        case SCHED_RR: return "SCHED_RR";
        case SCHED_BATCH: return "SCHED_BATCH";
        case SCHED_IDLE: return "SCHED_IDLE";
        default: return "unknown";
    }
}

void show(const char* when) {
    int policy = 0;
    sched_param sp{};
    pthread_getschedparam(pthread_self(), &policy, &sp);
    std::printf("   %-8s %s, priority %d\n", when, policy_name(policy), sp.sched_priority);
}

// Returns the pthread error number (0 on success). pthread functions do not
// set errno, so perror() would print a wrong message here.
int set_policy(int policy, int priority) {
    sched_param sp{};
    sp.sched_priority = priority;
    return pthread_setschedparam(pthread_self(), policy, &sp);
}

}  // namespace

int main(int argc, char** argv) {
    const int priority = argc > 1 ? std::atoi(argv[1]) : 50;

    std::printf("1. priority ranges\n");
    for (int p : {SCHED_OTHER, SCHED_FIFO, SCHED_RR}) {
        std::printf("   %-12s %2d .. %2d\n", policy_name(p), sched_get_priority_min(p),
                    sched_get_priority_max(p));
    }

    rlimit rl{};
    getrlimit(RLIMIT_RTPRIO, &rl);
    std::printf("\n2. RLIMIT_RTPRIO: soft %ld, hard %ld  (same as 'ulimit -r')\n",
                static_cast<long>(rl.rlim_cur), static_cast<long>(rl.rlim_max));
    show("now:");

    std::printf("\n3. switch to SCHED_FIFO %d\n", priority);
    int rc = set_policy(SCHED_FIFO, priority);
    if (rc != 0) {
        std::printf("   failed: %s\n", std::strerror(rc));
        if (rc == EPERM) {
            std::printf("   priority %d is above your limit of %ld\n", priority,
                        static_cast<long>(rl.rlim_cur));
        } else if (rc == EINVAL) {
            std::printf("   SCHED_FIFO priorities are 1..99\n");
        }
        return 1;
    }
    show("after:");

    // A SCHED_FIFO thread runs until it blocks or something of higher
    // priority wants the CPU. A bug such as while(true){} would starve every
    // normal process on that CPU. The kernel's RT throttling (see
    // /proc/sys/kernel/sched_rt_runtime_us) is the safety net.

    std::printf("\n4. back to SCHED_OTHER\n");
    rc = set_policy(SCHED_OTHER, 0);
    if (rc != 0) {
        std::printf("   failed: %s\n", std::strerror(rc));
        return 1;
    }
    show("after:");
    return 0;
}
