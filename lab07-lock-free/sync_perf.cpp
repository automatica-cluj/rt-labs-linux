// sync_perf.cpp - mutex, priority-inheritance mutex and std::atomic compared
//
// Two tests, each run for the three ways of protecting a shared counter:
//
//   1. throughput: several SCHED_OTHER threads increment the counter as fast
//      as they can for a fixed time, on as many CPUs as they like. Measures
//      the average cost of one increment.
//
//   2. mixed priority: three SCHED_FIFO threads pinned to ONE CPU.
//        high   priority 50, every 5 ms, one short update of the counter
//        medium priority 30, every 23 ms, 8 ms of pure computation
//        low    priority 10, every 7 ms, a 3 ms update of the counter
//      We record, for every period of the high thread, how long after its
//      release it finished its update. That is its response time; the max is
//      what a real-time designer cares about.
//
// With the plain mutex, high can wait for low, and low can in turn wait for
// medium: unbounded priority inversion. With PTHREAD_PRIO_INHERIT low runs at
// high's priority while it holds the lock. With std::atomic, low does its 3 ms
// of work on a private copy and publishes the result in one atomic step, so
// high never waits for anyone.
//
// Usage:  ./sync_perf [threads] [seconds_per_method] [cpu]
//   defaults: 4 threads, 2 s, a CPU picked from your uid

#include "rt.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {

enum class Method { kMutex, kPiMutex, kAtomic };
const char* method_name(Method m) {
    switch (m) {
        case Method::kMutex: return "mutex";
        case Method::kPiMutex: return "PI mutex";
        default: return "std::atomic";
    }
}

// One shared counter, protected in one of three ways.
class Counter {
public:
    explicit Counter(Method m)
        : method_(m), mutex_(m == Method::kPiMutex ? rt::Mutex::kInherit : rt::Mutex::kNone) {}

    // Do `work_ns` of computation that belongs to the update, then add 1.
    void update(long long work_ns) {
        if (method_ == Method::kAtomic) {
            // Compute outside, publish atomically. Nobody ever waits here.
            if (work_ns) rt::busy_ns(work_ns);
            atomic_.fetch_add(1);
        } else {
            // The computation is inside the critical section, as it would be
            // if it read and modified shared state.
            std::lock_guard<rt::Mutex> guard(mutex_);
            if (work_ns) rt::busy_ns(work_ns);
            ++plain_;
        }
    }

    // Uncontended fast path for the throughput test.
    void increment() {
        if (method_ == Method::kAtomic) {
            atomic_.fetch_add(1);
        } else {
            std::lock_guard<rt::Mutex> guard(mutex_);
            ++plain_;
        }
    }

    uint64_t value() const { return method_ == Method::kAtomic ? atomic_.load() : plain_; }

private:
    Method method_;
    rt::Mutex mutex_;
    uint64_t plain_ = 0;
    std::atomic<uint64_t> atomic_{0};
};

// ------------------------------------------------------------- test 1

// alignas(64): each thread's own `done` counter sits on its own cache line.
// Without it, neighbouring counters in the vector would share a cache line and
// slow each other down (false sharing), which would distort the comparison.
struct alignas(64) Bench {
    Counter* counter;
    pthread_barrier_t* start;
    std::atomic<bool>* stop;
    uint64_t done = 0;  // increments made by this thread
};

void* bench_worker(void* arg) {
    auto* b = static_cast<Bench*>(arg);
    pthread_barrier_wait(b->start);
    while (!b->stop->load(std::memory_order_relaxed)) {
        b->counter->increment();
        ++b->done;
    }
    return nullptr;
}

double throughput(Method m, int threads, int seconds, bool* correct) {
    Counter counter(m);
    std::atomic<bool> stop{false};
    pthread_barrier_t start;
    pthread_barrier_init(&start, nullptr, threads + 1);
    std::vector<Bench> bench(threads, Bench{&counter, &start, &stop});
    std::vector<pthread_t> ids(threads);
    for (int t = 0; t < threads; ++t)
        if (rt::start_thread(&ids[t], bench_worker, &bench[t], 0) != 0) std::exit(1);

    pthread_barrier_wait(&start);
    const timespec t0 = rt::now();
    rt::sleep_ms(seconds * 1000L);
    stop = true;
    for (pthread_t id : ids) pthread_join(id, nullptr);
    const double elapsed = rt::elapsed_ns(t0) / 1e9;
    pthread_barrier_destroy(&start);

    uint64_t total = 0;
    for (const Bench& b : bench) total += b.done;
    *correct = counter.value() == total;  // no increment lost
    return total / elapsed;
}

// ------------------------------------------------------------- test 2

struct Periodic {
    const char* name;
    Counter* counter;          // nullptr: the thread only computes
    long long period_ns;
    long long work_ns;
    std::atomic<bool>* stop;
    rt::Stats response;        // release -> end of work
    uint64_t updates = 0;
};

void* periodic_worker(void* arg) {
    auto* p = static_cast<Periodic*>(arg);
    timespec release = rt::now();
    while (!p->stop->load()) {
        if (p->counter) {
            p->counter->update(p->work_ns);
            ++p->updates;
        } else {
            rt::busy_ns(p->work_ns);
        }
        p->response.add(rt::elapsed_ns(release));
        rt::add_ns(release, p->period_ns);
        // Overrun policy: if we are already past the next release, skip the
        // periods we missed instead of running back to back. Without this a
        // late low-priority thread would hog the CPU while trying to catch up.
        const timespec t = rt::now();
        while (rt::diff_ns(release, t) < 0) rt::add_ns(release, p->period_ns);
        rt::sleep_until(release);
    }
    return nullptr;
}

struct MixedResult {
    rt::Stats high;
    bool correct;
};

MixedResult mixed_priority(Method m, int seconds, int cpu) {
    Counter counter(m);
    std::atomic<bool> stop{false};
    Periodic high{"high", &counter, 5 * rt::kNsPerMs, 50 * rt::kNsPerUs, &stop, {}};
    Periodic medium{"medium", nullptr, 23 * rt::kNsPerMs, 8 * rt::kNsPerMs, &stop, {}};
    Periodic low{"low", &counter, 7 * rt::kNsPerMs, 3 * rt::kNsPerMs, &stop, {}};

    pthread_t th_low, th_medium, th_high;
    if (rt::start_thread(&th_low, periodic_worker, &low, 10, cpu) != 0 ||
        rt::start_thread(&th_medium, periodic_worker, &medium, 30, cpu) != 0 ||
        rt::start_thread(&th_high, periodic_worker, &high, 50, cpu) != 0) {
        std::fprintf(stderr, "this test needs SCHED_FIFO priorities 10..50\n");
        std::exit(1);
    }

    rt::sleep_ms(seconds * 1000L);
    stop = true;
    pthread_join(th_high, nullptr);
    pthread_join(th_medium, nullptr);
    pthread_join(th_low, nullptr);

    return {high.response, counter.value() == high.updates + low.updates};
}

}  // namespace

int main(int argc, char** argv) {
    const int threads = static_cast<int>(rt::arg_long(argc, argv, 1, 4, 1, 64));
    const int seconds = static_cast<int>(rt::arg_long(argc, argv, 2, 2, 1, 60));
    const int cpu =
        static_cast<int>(rt::arg_long(argc, argv, 3, rt::default_cpu(), 0, CPU_SETSIZE - 1));
    const Method methods[] = {Method::kMutex, Method::kPiMutex, Method::kAtomic};
    bool all_correct = true;

    rt::lock_memory();

    std::printf("Test 1: throughput, %d SCHED_OTHER threads, %d s per method\n", threads, seconds);
    double best_rate = 0;
    Method best = Method::kMutex;
    for (Method m : methods) {
        bool correct = false;
        const double rate = throughput(m, threads, seconds, &correct);
        all_correct &= correct;
        std::printf("  %-12s %7.2f M increments/s  count %s\n", method_name(m), rate / 1e6,
                    correct ? "correct" : "WRONG");
        if (rate > best_rate) {
            best_rate = rate;
            best = m;
        }
    }
    std::printf("  highest throughput here: %s\n\n", method_name(best));

    std::printf("Test 2: mixed priorities on CPU %d, %d s per method\n", cpu, seconds);
    std::printf("  high: FIFO 50, 5 ms period, 50 us update\n");
    std::printf("  medium: FIFO 30, 23 ms period, 8 ms computation (no counter)\n");
    std::printf("  low: FIFO 10, 7 ms period, 3 ms update\n");
    long long best_max = -1;
    long long max_by_method[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        const MixedResult r = mixed_priority(methods[i], seconds, cpu);
        all_correct &= r.correct;
        char label[64];
        std::snprintf(label, sizeof(label), "  %-12s high response", method_name(methods[i]));
        r.high.print_us(label);
        if (!r.correct) std::printf("  %-12s count WRONG\n", method_name(methods[i]));
        max_by_method[i] = r.high.max;
        if (best_max < 0 || r.high.max < best_max) {
            best_max = r.high.max;
            best = methods[i];
        }
    }

    std::printf("\nWhat the numbers say on this run:\n");
    std::printf("  lowest worst-case response for high: %s (%lld us)\n", method_name(best),
                best_max / rt::kNsPerUs);
    const double ratio = static_cast<double>(max_by_method[0]) / max_by_method[1];
    if (ratio > 1.5) {
        std::printf("  plain mutex worst case is %.1fx the PI mutex worst case: priority\n"
                    "  inversion through the medium thread was observed.\n", ratio);
    } else {
        std::printf("  plain mutex and PI mutex worst cases are close (ratio %.1f): the bad\n"
                    "  interleaving did not happen in this run. Run longer.\n", ratio);
    }
    return all_correct ? 0 : 1;
}
