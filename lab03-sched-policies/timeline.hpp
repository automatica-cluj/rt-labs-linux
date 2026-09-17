// timeline.hpp - run CPU-bound workers and record *when* each one ran
//
// All three programs in this lab use the same trick. A worker burns a fixed
// amount of CPU time (measured with CLOCK_THREAD_CPUTIME_ID, so waiting does
// not count as work). Every few microseconds it looks at the clock. If more
// than kGapNs passed since the last look, somebody else had the CPU: the
// worker closes its current "piece" of execution and starts a new one.
//
// After all workers finish, main prints:
//   - requested versus obtained policy and priority (never trust the request)
//   - first run, finish time and number of pieces per worker
//   - a text strip showing which worker owned the CPU over time
//
// All workers are pinned to ONE CPU. On a multi-core machine unpinned threads
// simply run in parallel on different CPUs and no scheduling policy has
// anything to decide. Scheduling policies only matter when tasks compete for
// the same CPU. Passing cpu -1 turns pinning off, so you can see that.

#pragma once

#include "rt.hpp"

#include <semaphore.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace lab {

constexpr long long kGapNs = 500 * rt::kNsPerUs;  // a pause this long = preempted
constexpr int kMaxPieces = 512;

struct Piece {
    long long start_ns;
    long long end_ns;
};

struct Worker {
    // set by main
    const char* name = "";
    char tag = '?';
    int policy = SCHED_OTHER;
    int priority = 0;
    int cpu = -1;
    long long work_ns = 0;
    timespec t0{};          // common time origin, set just before release
    sem_t* parked = nullptr;  // worker posts here when it is ready
    sem_t go{};               // main posts here to release the worker

    // filled in by the worker itself
    int got_policy = -1;
    int got_priority = -1;
    int got_cpu = -1;
    Piece pieces[kMaxPieces]{};
    int npieces = 0;
    long long first_run_ns = 0;
    long long finish_ns = 0;
    pthread_t thread{};
};

inline long long thread_cpu_ns() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<long long>(t.tv_sec) * rt::kNsPerSec + t.tv_nsec;
}

// Thread body. No printf in here: output to a terminal can block and would
// change the very scheduling we are trying to observe.
inline void* worker_main(void* arg) {
    auto* w = static_cast<Worker*>(arg);

    sched_param sp{};
    pthread_getschedparam(pthread_self(), &w->got_policy, &sp);
    w->got_priority = sp.sched_priority;
    w->got_cpu = sched_getcpu();

    // Park until main releases us. Waking a blocked task puts it at the
    // back of the queue for its priority, so the release order is known.
    sem_post(w->parked);
    while (sem_wait(&w->go) != 0) {
    }

    const long long cpu_start = thread_cpu_ns();
    long long last = rt::diff_ns(rt::now(), w->t0);
    w->first_run_ns = last;
    w->pieces[0].start_ns = last;
    w->npieces = 1;

    volatile unsigned long x = 0;
    while (thread_cpu_ns() - cpu_start < w->work_ns) {
        for (int i = 0; i < 2000; ++i) x += i;  // about 1-2 us of "work"
        const long long t = rt::diff_ns(rt::now(), w->t0);
        if (t - last > kGapNs && w->npieces < kMaxPieces) {
            w->pieces[w->npieces - 1].end_ns = last;
            w->pieces[w->npieces].start_ns = t;
            ++w->npieces;
        }
        last = t;
    }
    w->pieces[w->npieces - 1].end_ns = last;
    w->finish_ns = last;
    return nullptr;
}

inline const char* short_policy(int policy) {
    switch (policy) {
        case SCHED_FIFO: return "FIFO";
        case SCHED_RR: return "RR";
        case SCHED_OTHER: return "OTHER";
        default: return "?";
    }
}

// Main thread: optionally become a real-time task one step above the workers
// on the same CPU. Workers cannot run while main is running, so main controls
// exactly when and in which order they become runnable.
inline void setup_main(int priority, int cpu) {
    if (!rt::lock_memory()) std::exit(1);
    if (cpu >= 0 && rt::pin_self_to_cpu(cpu) != 0) std::exit(1);
    if (priority > 0 && rt::set_self_sched(priority, SCHED_FIFO) != 0) std::exit(1);
    rt::print_self_sched("main");
}

// Create all workers, wait until every one is parked, then release them in
// vector order and wait for them to finish.
inline void start_all(std::vector<Worker>& workers) {
    sem_t parked;
    sem_init(&parked, 0, 0);
    for (auto& w : workers) {
        w.parked = &parked;
        sem_init(&w.go, 0, 0);
        // Explicit scheduling attributes: the thread is real-time from its
        // very first instruction. A failure (EPERM) is fatal on purpose.
        if (rt::start_thread(&w.thread, worker_main, &w, w.priority, w.cpu, w.policy) != 0) {
            std::exit(1);
        }
    }
    for (size_t i = 0; i < workers.size(); ++i) {
        while (sem_wait(&parked) != 0) {
        }
    }
    // Release. Main keeps the CPU until it blocks in pthread_join, so all
    // workers become runnable before any of them starts working.
    const timespec t0 = rt::now();
    for (auto& w : workers) w.t0 = t0;
    for (auto& w : workers) sem_post(&w.go);
    for (auto& w : workers) pthread_join(w.thread, nullptr);
    for (auto& w : workers) sem_destroy(&w.go);
    sem_destroy(&parked);
}

inline std::string order_by(const std::vector<Worker>& workers, long long Worker::*key) {
    std::vector<const Worker*> v;
    for (const auto& w : workers) v.push_back(&w);
    std::stable_sort(v.begin(), v.end(),
                     [key](const Worker* a, const Worker* b) { return a->*key < b->*key; });
    std::string s;
    for (const auto* w : v) {
        if (!s.empty()) s += ' ';
        s += w->tag;
    }
    return s;
}

inline int max_pieces(const std::vector<Worker>& workers) {
    int m = 0;
    for (const auto& w : workers) m = std::max(m, w.npieces);
    return m;
}

// Did the workers take turns? True if some worker ran (a piece) between the
// first and last piece of another worker. Pauses caused by tasks that are not
// ours (kernel threads, the fair server, RT throttling) do not count.
inline bool interleaved(const std::vector<Worker>& workers) {
    for (const auto& a : workers) {
        for (const auto& b : workers) {
            if (&a == &b) continue;
            for (int i = 0; i < b.npieces; ++i) {
                if (b.pieces[i].start_ns > a.first_run_ns && b.pieces[i].start_ns < a.finish_ns) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Did the workers run at the same time on different CPUs?
inline bool parallel(const std::vector<Worker>& workers) {
    for (const auto& w : workers) {
        if (w.got_cpu != workers[0].got_cpu) return true;
    }
    return false;
}

inline const char* cpu_text(int cpu) {
    static char buf[32];
    if (cpu < 0) return "any CPU (not pinned)";
    std::snprintf(buf, sizeof buf, "CPU %d", cpu);
    return buf;
}

inline void report(const std::vector<Worker>& workers) {
    std::printf("\n%-8s %-13s %-13s %4s %9s %9s %6s\n", "worker", "requested", "obtained", "cpu",
                "first ms", "done ms", "pieces");
    bool mismatch = false;
    for (const auto& w : workers) {
        char req[32], got[32];
        std::snprintf(req, sizeof req, "%s %d", short_policy(w.policy), w.priority);
        std::snprintf(got, sizeof got, "%s %d", short_policy(w.got_policy), w.got_priority);
        if (w.policy != w.got_policy || w.priority != w.got_priority) mismatch = true;
        std::printf("%c %-6s %-13s %-13s %4d %9.1f %9.1f %6d\n", w.tag, w.name, req, got, w.got_cpu,
                    w.first_run_ns / 1e6, w.finish_ns / 1e6, w.npieces);
    }
    if (mismatch) std::printf("WARNING: some worker did not get the policy it asked for\n");

    // Text strip: each column is a slice of time, the letter is the worker
    // that ran in that slice, '*' means several of ours ran at the same time
    // (only possible on different CPUs), '.' means none of ours ran.
    long long end = 0;
    for (const auto& w : workers) end = std::max(end, w.finish_ns);
    const int cols = 64;
    const long long width = std::max(1LL, end / cols + 1);
    std::string strip(cols, '.');
    for (int c = 0; c < cols; ++c) {
        const long long a = c * width, b = a + width;
        long long best = 0;
        int busy = 0;
        for (const auto& w : workers) {
            long long run = 0;
            for (int i = 0; i < w.npieces; ++i) {
                run += std::max(0LL, std::min(b, w.pieces[i].end_ns) -
                                         std::max(a, w.pieces[i].start_ns));
            }
            if (run * 2 > width) ++busy;  // ran for most of the slice
            if (run > best && run * 4 > width) {
                best = run;
                strip[c] = w.tag;
            }
        }
        if (busy > 1) strip[c] = '*';
    }
    std::printf("\ntimeline, one column = %.1f ms:\n  |%s|\n", width / 1e6, strip.c_str());
    std::printf("start order:  %s\n", order_by(workers, &Worker::first_run_ns).c_str());
    std::printf("finish order: %s\n", order_by(workers, &Worker::finish_ns).c_str());
    std::printf("most pieces for one worker: %d\n", max_pieces(workers));
    std::printf("workers interleaved: %s\n", interleaved(workers) ? "yes" : "no");
    if (parallel(workers)) {
        std::printf("NOTE: workers ran on different CPUs, in parallel. The scheduling policy\n"
                    "      had nothing to decide. Pin them to one CPU to see it at work.\n");
    }
}

}  // namespace lab
