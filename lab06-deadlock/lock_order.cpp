// lock_order.cpp - preventing the deadlock of deadlock.cpp
//
// Same two tasks and the same two mutexes. Task A always takes m1 then m2.
// Task B would naturally take m2 first. The mode decides how B does it:
//
//   ordered  B follows the global order m1 -> m2. No cycle is possible.
//   trylock  B takes m2, then *tries* m1. If m1 is busy, B releases m2,
//            waits a little and starts over (breaks "hold and wait").
//   scoped   B uses std::scoped_lock(m2, m1). The standard library locks both
//            with a try-and-back-off algorithm, so the order written does
//            not matter.
//   wrong    B takes m2 then m1 with plain locks: the deadlock comes back and
//            the watchdog reports it.
//
// Usage:  ./lock_order [mode] [priority]
//   mode      ordered (default) | trylock | scoped | wrong
//   priority  0 = SCHED_OTHER (default), 1..80 = SCHED_FIFO for both tasks
//
// Timeline (times from program start):
//     0 ms  A locks m1, works 100 ms
//    50 ms  B starts and needs both mutexes
//   100 ms  A locks m2, works 50 ms
//   150 ms  A releases both; B can now get both and works 50 ms

#include "rt.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

namespace {

constexpr long kHoldMs = 100;       // A's work holding only m1
constexpr long kWorkMs = 50;        // work holding both
constexpr long kBStartMs = 50;
constexpr long kBackoffMs = 5;      // trylock mode: pause before retrying
constexpr long kWatchdogMs = 3000;

enum class Mode { kOrdered, kTrylock, kScoped, kWrong };

timespec g_start;
Mode g_mode = Mode::kOrdered;

// rt::Mutex plus try_lock, so std::scoped_lock can use it.
class LockableMutex : public rt::Mutex {
public:
    bool try_lock() {
        int rc = pthread_mutex_trylock(native());
        if (rc == 0) return true;
        if (rc != EBUSY) {
            std::fprintf(stderr, "pthread_mutex_trylock: %s\n", std::strerror(rc));
            std::abort();
        }
        return false;
    }
};

LockableMutex g_m1;  // lock order: 1
LockableMutex g_m2;  // lock order: 2
int g_resource1 = 0;
int g_resource2 = 0;

struct Result {
    double started_ms = 0;
    double got_both_ms = 0;
    double finished_ms = 0;
    int retries = 0;
    std::atomic<bool> done{false};
};

Result g_a, g_b;

void log(char task, const char* what) {
    std::printf("[%6.1f ms] task %c: %s\n", rt::elapsed_ms(g_start), task, what);
    std::fflush(stdout);
}

void critical_section(char task, int add) {
    g_resource1 += add;
    g_resource2 += add;
    log(task, "has m1 and m2, working");
    rt::sleep_ms(kWorkMs);
}

void* task_a(void*) {
    g_a.started_ms = rt::elapsed_ms(g_start);
    std::lock_guard<LockableMutex> l1(g_m1);
    log('A', "got m1");
    rt::sleep_ms(kHoldMs);
    log('A', "locking m2");
    std::lock_guard<LockableMutex> l2(g_m2);
    g_a.got_both_ms = rt::elapsed_ms(g_start);
    critical_section('A', 1);
    g_a.finished_ms = rt::elapsed_ms(g_start);
    log('A', "releasing both");
    g_a.done = true;
    return nullptr;
}

void* task_b(void*) {
    rt::sleep_ms(kBStartMs);
    g_b.started_ms = rt::elapsed_ms(g_start);

    switch (g_mode) {
        case Mode::kOrdered: {
            log('B', "locking m1 then m2 (global order)");
            std::lock_guard<LockableMutex> l1(g_m1);
            std::lock_guard<LockableMutex> l2(g_m2);
            g_b.got_both_ms = rt::elapsed_ms(g_start);
            critical_section('B', 10);
            break;
        }
        case Mode::kTrylock: {
            log('B', "locking m2, then trying m1");
            for (;;) {
                g_m2.lock();
                if (g_m1.try_lock()) break;
                g_m2.unlock();  // do not hold m2 while waiting for m1
                ++g_b.retries;
                rt::sleep_ms(kBackoffMs);
            }
            g_b.got_both_ms = rt::elapsed_ms(g_start);
            critical_section('B', 10);
            g_m1.unlock();
            g_m2.unlock();
            break;
        }
        case Mode::kScoped: {
            log('B', "std::scoped_lock(m2, m1)");
            std::scoped_lock both(g_m2, g_m1);
            g_b.got_both_ms = rt::elapsed_ms(g_start);
            critical_section('B', 10);
            break;
        }
        case Mode::kWrong: {
            log('B', "locking m2 then m1 (WRONG order)");
            std::lock_guard<LockableMutex> l2(g_m2);
            log('B', "got m2, waiting for m1");
            std::lock_guard<LockableMutex> l1(g_m1);
            g_b.got_both_ms = rt::elapsed_ms(g_start);
            critical_section('B', 10);
            break;
        }
    }
    g_b.finished_ms = rt::elapsed_ms(g_start);
    log('B', "released both");
    g_b.done = true;
    return nullptr;
}

void report(char name, const Result& r) {
    std::printf("  task %c: started %5.1f ms, held both from %5.1f ms, finished %5.1f ms, "
                "run time %5.1f ms",
                name, r.started_ms, r.got_both_ms, r.finished_ms, r.finished_ms - r.started_ms);
    if (name == 'B' && g_mode == Mode::kTrylock) std::printf(", %d retries", r.retries);
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "ordered";
    if (!std::strcmp(mode, "ordered")) g_mode = Mode::kOrdered;
    else if (!std::strcmp(mode, "trylock")) g_mode = Mode::kTrylock;
    else if (!std::strcmp(mode, "scoped")) g_mode = Mode::kScoped;
    else if (!std::strcmp(mode, "wrong")) g_mode = Mode::kWrong;
    else {
        std::fprintf(stderr, "mode must be ordered, trylock, scoped or wrong\n");
        return 2;
    }
    const int priority = static_cast<int>(rt::arg_long(argc, argv, 2, 0, 0, 99));
    std::printf("mode: %s, priority: %d\n\n", mode, priority);

    g_start = rt::now();
    pthread_t ta, tb;
    if (rt::start_thread(&ta, task_a, nullptr, priority) != 0) return 1;
    if (rt::start_thread(&tb, task_b, nullptr, priority) != 0) return 1;

    while (!(g_a.done && g_b.done) && rt::elapsed_ms(g_start) < kWatchdogMs) rt::sleep_ms(10);
    if (!(g_a.done && g_b.done)) {
        std::printf("\nDEADLOCK: both threads stuck after %.0f ms (watchdog)\n",
                    rt::elapsed_ms(g_start));
        std::fflush(stdout);
        _exit(3);
    }
    pthread_join(ta, nullptr);
    pthread_join(tb, nullptr);

    std::printf("\nresult\n");
    report('A', g_a);
    report('B', g_b);
    std::printf("  total %.1f ms, resource1 = %d, resource2 = %d (expected 11 and 11)\n",
                rt::elapsed_ms(g_start), g_resource1, g_resource2);
    std::printf("  completed, no deadlock\n");
    return 0;
}
