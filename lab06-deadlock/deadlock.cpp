// deadlock.cpp - two tasks, two mutexes, opposite lock order
//
//   Task A: lock m1, work, then lock m2
//   Task B: lock m2, work, then lock m1      <- opposite order: circular wait
//
// With a timeout (the default) each second lock uses pthread_mutex_clocklock
// on CLOCK_MONOTONIC. A task that times out treats it as a deadlock: it gives
// back the mutex it holds, backs off and tries again.
//
// With timeout 0 both tasks use a plain pthread_mutex_lock and block forever.
// A watchdog in main notices that nobody finished and ends the program, so
// your terminal never hangs.
//
// Usage:  ./deadlock [timeout_ms] [jitter_ms] [priority]
//   timeout_ms  0 = no timeout (real deadlock), default 1000
//   jitter_ms   random extra delay 0..jitter before the second lock, default 0
//   priority    0 = SCHED_OTHER (default), 1..80 = SCHED_FIFO for both tasks
//
// With jitter 0 the timeline is fixed (times from program start):
//     0 ms  A locks m1
//    50 ms  B locks m2
//   100 ms  A waits for m2       (deadline at 100 + timeout)
//   150 ms  B waits for m1       (deadline at 150 + timeout)
//  1100 ms  A times out, releases m1 and backs off
//  1100 ms  B gets m1 at once (it never times out), works, releases both
//  1300 ms  A tries again and succeeds

#include "rt.hpp"

#include <atomic>
#include <random>

namespace {

constexpr int kMaxAttempts = 3;
constexpr long kHoldMs = 100;   // work done while holding the first mutex
constexpr long kWorkMs = 50;    // work done while holding both
constexpr long kWatchdogExtraMs = 2000;

timespec g_start;
long g_timeout_ms = 1000;
long g_jitter_ms = 0;

rt::Mutex g_m1;
rt::Mutex g_m2;
int g_resource1 = 0;  // protected by m1
int g_resource2 = 0;  // protected by m2

// Printed at the end, and by the watchdog if the tasks are stuck.
std::atomic<char> g_m1_owner{'-'};
std::atomic<char> g_m2_owner{'-'};

struct Task {
    char name;               // 'A' or 'B'
    rt::Mutex* first;
    rt::Mutex* second;
    std::atomic<char>* first_owner;
    std::atomic<char>* second_owner;
    const char* first_name;
    const char* second_name;
    long start_delay_ms;
    long backoff_ms;
    int add1, add2;          // what the task adds to the two resources
    // results
    std::atomic<int> attempts{0};
    int timeouts = 0;
    std::atomic<bool> done{false};    // finished its work
    std::atomic<bool> exited{false};  // left run_task, done or gave up
    double finished_at_ms = 0;
};

void log(const Task& t, const char* what, const char* mutex = "") {
    std::printf("[%7.1f ms] task %c: %s%s\n", rt::elapsed_ms(g_start), t.name, what, mutex);
    std::fflush(stdout);
}

void die(const char* what, int rc) {
    std::fprintf(stderr, "%s: %s\n", what, std::strerror(rc));
    std::abort();
}

void lock_or_die(rt::Mutex* m) {
    int rc = pthread_mutex_lock(m->native());
    if (rc != 0) die("pthread_mutex_lock", rc);
}

void unlock_or_die(rt::Mutex* m) {
    int rc = pthread_mutex_unlock(m->native());
    if (rc != 0) die("pthread_mutex_unlock", rc);
}

// Lock with a deadline. Returns true on success, false on ETIMEDOUT.
// Any other error is a bug in the program, not a deadlock: stop.
bool lock_with_timeout(rt::Mutex* m, long timeout_ms) {
    if (timeout_ms == 0) {
        lock_or_die(m);
        return true;
    }
    timespec deadline = rt::now();
    rt::add_ns(deadline, timeout_ms * rt::kNsPerMs);
    int rc = pthread_mutex_clocklock(m->native(), CLOCK_MONOTONIC, &deadline);
    if (rc == 0) return true;
    if (rc == ETIMEDOUT) return false;
    die("pthread_mutex_clocklock", rc);
    return false;
}

void* run_task(void* arg) {
    Task& t = *static_cast<Task*>(arg);
    std::mt19937 rng(std::random_device{}());

    rt::sleep_ms(t.start_delay_ms);

    while (t.attempts < kMaxAttempts) {
        ++t.attempts;

        log(t, "locking ", t.first_name);
        lock_or_die(t.first);
        t.first_owner->store(t.name);
        log(t, "got ", t.first_name);

        long hold = kHoldMs;
        if (g_jitter_ms > 0) hold += std::uniform_int_distribution<long>(0, g_jitter_ms)(rng);
        rt::sleep_ms(hold);

        log(t, "waiting for ", t.second_name);
        if (!lock_with_timeout(t.second, g_timeout_ms)) {
            ++t.timeouts;
            log(t, "TIMEOUT, assuming deadlock, releasing ", t.first_name);
            t.first_owner->store('-');
            unlock_or_die(t.first);
            rt::sleep_ms(t.backoff_ms);
            continue;
        }
        t.second_owner->store(t.name);
        log(t, "got ", t.second_name);

        g_resource1 += t.add1;
        g_resource2 += t.add2;
        rt::sleep_ms(kWorkMs);

        t.second_owner->store('-');
        unlock_or_die(t.second);
        t.first_owner->store('-');
        unlock_or_die(t.first);
        t.finished_at_ms = rt::elapsed_ms(g_start);
        log(t, "done, released both");
        t.done = true;
        t.exited = true;
        return nullptr;
    }
    log(t, "GAVE UP after max attempts");
    t.exited = true;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    g_timeout_ms = rt::arg_long(argc, argv, 1, 1000, 0, 60000);
    g_jitter_ms = rt::arg_long(argc, argv, 2, 0, 0, 10000);
    const int priority = static_cast<int>(rt::arg_long(argc, argv, 3, 0, 0, 99));

    // Worst case for one task: every attempt waits hold + jitter + timeout + backoff.
    const long watchdog_ms =
        g_timeout_ms == 0
            ? kHoldMs + g_jitter_ms + kWatchdogExtraMs
            : kMaxAttempts * (kHoldMs + g_jitter_ms + g_timeout_ms + 300 + kWorkMs) +
                  kWatchdogExtraMs;

    if (g_timeout_ms == 0) {
        std::printf("timeout: none (plain pthread_mutex_lock), watchdog after %ld ms\n",
                    watchdog_ms);
    } else {
        std::printf("timeout: %ld ms (pthread_mutex_clocklock, CLOCK_MONOTONIC)\n", g_timeout_ms);
    }
    std::printf("jitter: %ld ms, priority: %d\n\n", g_jitter_ms, priority);

    Task a;
    a.name = 'A';
    a.first = &g_m1, a.second = &g_m2;
    a.first_owner = &g_m1_owner, a.second_owner = &g_m2_owner;
    a.first_name = "m1", a.second_name = "m2";
    a.start_delay_ms = 0, a.backoff_ms = 200;
    a.add1 = 1, a.add2 = 1;

    Task b;
    b.name = 'B';
    b.first = &g_m2, b.second = &g_m1;  // opposite order
    b.first_owner = &g_m2_owner, b.second_owner = &g_m1_owner;
    b.first_name = "m2", b.second_name = "m1";
    b.start_delay_ms = 50, b.backoff_ms = 300;  // different back-off avoids livelock
    b.add1 = 10, b.add2 = 10;

    g_start = rt::now();
    pthread_t ta, tb;
    if (rt::start_thread(&ta, run_task, &a, priority) != 0) return 1;
    if (rt::start_thread(&tb, run_task, &b, priority) != 0) return 1;

    // Watchdog: poll instead of joining, because a deadlocked thread never returns.
    while (!(a.exited && b.exited) && rt::elapsed_ms(g_start) < watchdog_ms) {
        rt::sleep_ms(10);
    }

    if (!(a.done && b.done)) {
        const bool stuck = g_m1_owner != '-' && g_m2_owner != '-';
        std::printf("\n%s after %.0f ms\n",
                    stuck ? "DEADLOCK: both threads stuck" : "NOT FINISHED: a task gave up",
                    rt::elapsed_ms(g_start));
        std::printf("  m1 held by task %c, m2 held by task %c\n", g_m1_owner.load(),
                    g_m2_owner.load());
        std::fflush(stdout);
        // Stuck threads can never be joined; leave without running destructors.
        _exit(stuck ? 3 : 4);
    }

    pthread_join(ta, nullptr);
    pthread_join(tb, nullptr);

    std::printf("\nresult\n");
    std::printf("  task A: %d attempt(s), %d timeout(s), finished at %.0f ms\n", a.attempts.load(),
                a.timeouts, a.finished_at_ms);
    std::printf("  task B: %d attempt(s), %d timeout(s), finished at %.0f ms\n", b.attempts.load(),
                b.timeouts, b.finished_at_ms);
    std::printf("  resource1 = %d, resource2 = %d (expected 11 and 11)\n", g_resource1,
                g_resource2);
    if (a.timeouts + b.timeouts > 0) {
        std::printf("  deadlock detected %d time(s) by timeout and recovered\n",
                    a.timeouts + b.timeouts);
    } else {
        std::printf("  no timeout fired: the tasks did not overlap this time\n");
    }
    return 0;
}
