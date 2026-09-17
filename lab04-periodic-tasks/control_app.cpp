// control_app.cpp - three periodic tasks under Rate Monotonic Scheduling
//
// A small control system, all three threads pinned to the same CPU:
//
//   task        period  computation  role
//   sensor        5 ms       800 us  reads a (simulated) sensor value
//   controller   10 ms      2000 us  computes an output from the sensor value
//   logger       20 ms      3000 us  takes a consistent snapshot of both
//
// Rate Monotonic: the shorter the period, the higher the priority. With
// priority P on the command line the tasks get P, P-10 and P-20.
//
// Before running, the program does the schedulability analysis:
//   - total utilisation U against the Liu & Layland bound n(2^(1/n) - 1)
//   - exact response-time analysis R = C + sum over higher tasks ceil(R/T)*C
// and afterwards compares the predicted worst case with what it measured.
//
// The tasks share data through a mutex with priority inheritance
// (rt::Mutex::kInherit), the right default for any lock shared by real-time
// threads of different priorities (see lab 5).
//
// Usage:  ./control_app [priority] [seconds] [cpu] [load_percent] [reverse]
//   defaults: 80 5 <per-user cpu> 100 0
//   priority      0 = all SCHED_OTHER, otherwise 21..80 (three levels needed)
//   cpu          -1 = do not pin (the tasks then run in parallel)
//   load_percent  scales every computation time: 150 = 1.5 x the table above
//   reverse       1 = give the LONGEST period the highest priority (not RMS)
//
// Ctrl+C stops early; statistics cover the jobs that actually ran.

#include "rt.hpp"

#include <signal.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

// Data flowing sensor -> controller -> logger.
struct Plant {
    rt::Mutex lock{rt::Mutex::kInherit};
    int sensor_value = 0;
    int control_output = 0;
    long sensor_updates = 0;
};

Plant g_plant;

struct Task;
using Body = void (*)(Task&);

struct Task {
    Task(const char* n, long period, long wcet, Body b)
        : name(n), period_us(period), wcet_us(wcet), body(b) {}

    const char* name;
    long period_us;
    long wcet_us;
    Body body;
    int priority = 0;
    int cpu = -1;
    timespec first_release{};
    timespec end{};

    // results, written only by the task's own thread
    rt::Stats latency;
    rt::Stats response;
    long jobs = 0;
    long misses = 0;
    long skipped = 0;
    // logger only: last consistent snapshot
    int seen_sensor = 0;
    int seen_output = 0;
};

void sensor_body(Task& t) {
    rt::busy_ns(t.wcet_us * rt::kNsPerUs);
    std::lock_guard<rt::Mutex> guard(g_plant.lock);
    g_plant.sensor_value = (g_plant.sensor_value + 1) % 1000;
    ++g_plant.sensor_updates;
}

void controller_body(Task& t) {
    int input;
    {
        std::lock_guard<rt::Mutex> guard(g_plant.lock);
        input = g_plant.sensor_value;
    }
    rt::busy_ns(t.wcet_us * rt::kNsPerUs);  // the control algorithm
    std::lock_guard<rt::Mutex> guard(g_plant.lock);
    g_plant.control_output = 2 * input;
}

void logger_body(Task& t) {
    {
        // Keep critical sections short: copy, then do the slow work unlocked.
        std::lock_guard<rt::Mutex> guard(g_plant.lock);
        t.seen_sensor = g_plant.sensor_value;
        t.seen_output = g_plant.control_output;
    }
    rt::busy_ns(t.wcet_us * rt::kNsPerUs);  // formatting, writing to storage...
}

// The periodic loop, same structure and overrun policy as periodic_task.cpp.
void* run_task(void* arg) {
    Task& t = *static_cast<Task*>(arg);
    const long long period_ns = t.period_us * rt::kNsPerUs;
    timespec release = t.first_release;

    while (!g_stop.load(std::memory_order_relaxed) && rt::diff_ns(release, t.end) < 0) {
        rt::sleep_until(release);
        const timespec woke = rt::now();
        t.body(t);
        const timespec done = rt::now();

        const long long response = rt::diff_ns(done, release);
        t.latency.add(rt::diff_ns(woke, release));
        t.response.add(response);
        if (response > period_ns) ++t.misses;
        ++t.jobs;

        rt::add_ns(release, period_ns);
        const long long behind = rt::diff_ns(done, release);
        if (behind > 0) {
            const long long lost = behind / period_ns + 1;
            t.skipped += lost;
            rt::add_ns(release, lost * period_ns);
        }
    }
    return nullptr;
}

// Exact response-time analysis for fixed priorities on one CPU.
// Returns the worst-case response time in us, or -1 if it exceeds the period.
long response_time_bound(const Task tasks[], int n, int i) {
    long r = tasks[i].wcet_us;
    for (int iter = 0; iter < 1000; ++iter) {
        long next = tasks[i].wcet_us;
        for (int j = 0; j < n; ++j) {
            if (j != i && tasks[j].priority > tasks[i].priority) {
                next += static_cast<long>(std::ceil(static_cast<double>(r) / tasks[j].period_us)) *
                        tasks[j].wcet_us;
            }
        }
        if (next > tasks[i].period_us) return -1;
        if (next == r) return r;
        r = next;
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    const int priority = static_cast<int>(rt::arg_long(argc, argv, 1, 80, 0, 99));
    const int seconds = static_cast<int>(rt::arg_long(argc, argv, 2, 5, 1, 3600));
    const int cpu = static_cast<int>(rt::arg_long(argc, argv, 3, rt::default_cpu(), -1, CPU_SETSIZE - 1));
    const long load = rt::arg_long(argc, argv, 4, 100, 1, 1000);
    const bool reverse = rt::arg_long(argc, argv, 5, 0, 0, 1) == 1;

    if (priority != 0 && priority < 21) {
        std::fprintf(stderr, "priority must be 0 or at least 21 (the tasks use P, P-10, P-20)\n");
        return 2;
    }

    constexpr int kTasks = 3;
    Task tasks[kTasks] = {
        Task("sensor", 5'000, 800 * load / 100, sensor_body),
        Task("controller", 10'000, 2'000 * load / 100, controller_body),
        Task("logger", 20'000, 3'000 * load / 100, logger_body),
    };
    for (int i = 0; i < kTasks; ++i) {
        const int level = reverse ? kTasks - 1 - i : i;
        tasks[i].priority = priority == 0 ? 0 : priority - 10 * level;
        tasks[i].cpu = cpu;
    }

    // ---- analysis before running
    double u = 0;
    for (const Task& t : tasks) u += static_cast<double>(t.wcet_us) / t.period_us;
    const double bound = kTasks * (std::pow(2.0, 1.0 / kTasks) - 1.0);

    std::printf("%-10s %8s %8s %6s %8s %12s\n", "task", "period", "wcet", "U", "priority",
                "R bound");
    for (int i = 0; i < kTasks; ++i) {
        const long rb = response_time_bound(tasks, kTasks, i);
        char rbuf[32];
        if (rb < 0) std::snprintf(rbuf, sizeof rbuf, "> period");
        else std::snprintf(rbuf, sizeof rbuf, "%ld us", rb);
        std::printf("%-10s %5ld ms %5ld us %5.1f%% %8d %12s\n", tasks[i].name,
                    tasks[i].period_us / 1000, tasks[i].wcet_us,
                    100.0 * tasks[i].wcet_us / tasks[i].period_us, tasks[i].priority, rbuf);
    }
    std::printf("total utilisation %.1f %%, Liu & Layland bound for %d tasks %.1f %%: %s\n",
                100 * u, kTasks, 100 * bound,
                u <= bound ? "schedulable by the bound"
                           : (u <= 1.0 ? "bound inconclusive, see R bound column" : "overloaded"));
    if (priority == 0) std::printf("priority 0: all tasks SCHED_OTHER, the analysis does not apply\n");
    if (cpu < 0) std::printf("cpu -1: tasks not pinned, the analysis (one CPU) does not apply\n");
    if (reverse) std::printf("reverse: priorities are NOT rate monotonic\n");

    // ---- setup
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (!rt::lock_memory()) return 1;

    // Common first release (a "critical instant": all tasks released together).
    timespec start = rt::now();
    rt::add_ns(start, 50 * rt::kNsPerMs);
    timespec end = start;
    rt::add_ns(end, static_cast<long long>(seconds) * rt::kNsPerSec);

    pthread_t threads[kTasks];
    int created = 0;
    for (int i = 0; i < kTasks; ++i) {
        tasks[i].first_release = start;
        tasks[i].end = end;
        if (rt::start_thread(&threads[i], run_task, &tasks[i], tasks[i].priority, tasks[i].cpu) != 0) {
            g_stop.store(true);
            break;
        }
        ++created;
    }
    for (int i = 0; i < created; ++i) pthread_join(threads[i], nullptr);
    if (created != kTasks) return 1;

    // ---- results
    std::printf("\nrunning on CPU %d for %d s%s\n", cpu, seconds,
                g_stop.load() ? " (stopped early by signal)" : "");
    std::printf("%-10s %6s %7s %7s %13s %13s %13s\n", "task", "jobs", "misses", "skipped",
                "max latency", "avg response", "max response");
    long total_misses = 0;
    for (const Task& t : tasks) {
        total_misses += t.misses;
        std::printf("%-10s %6ld %7ld %7ld %10lld us %10lld us %10lld us\n", t.name, t.jobs, t.misses,
                    t.skipped, t.latency.max / rt::kNsPerUs, t.response.avg() / rt::kNsPerUs,
                    t.response.max / rt::kNsPerUs);
    }
    std::printf("logger's last snapshot: sensor %d, output %d (%ld sensor updates)\n",
                tasks[2].seen_sensor, tasks[2].seen_output, g_plant.sensor_updates);
    if (total_misses == 0) std::printf("RESULT: all deadlines met\n");
    else std::printf("RESULT: %ld deadline misses\n", total_misses);
    return 0;
}
