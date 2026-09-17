// periodic_task.cpp - one periodic task with a deadline
//
// A control loop is released every `period`, computes for `wcet`, and must be
// finished before its next release (implicit deadline = period). The program:
//   1. locks its memory and pins itself to one CPU
//   2. switches to SCHED_FIFO at the given priority (0 = SCHED_OTHER)
//   3. runs the loop with an absolute timer, recording for every job:
//        release latency   how late the job started   (wake-up - release)
//        response time     how long until it finished (completion - release)
//        deadline miss     response time > period
//   4. prints a summary at the end. Nothing is printed inside the loop:
//      printf can block on the terminal and cause the misses we measure.
//
// Overrun policy: if a job finishes after one or more later releases have
// already passed, those releases are skipped (counted, not run late) and the
// task re-synchronises with the next release still in the future. Without
// this, a late task runs a burst of back-to-back catch-up jobs.
//
// Usage:  ./periodic_task [priority] [seconds] [cpu] [period_us] [wcet_us]
//   defaults: 80 5 <per-user cpu> 10000 2000   (cpu -1 = do not pin)

#include "rt.hpp"

#include <cstdio>

namespace {

constexpr int kMaxRecordedMisses = 10;

struct Result {
    rt::Stats latency;
    rt::Stats response;
    long jobs = 0;
    long misses = 0;
    long skipped = 0;
    long miss_jobs[kMaxRecordedMisses] = {};  // job numbers of the first misses
};

}  // namespace

int main(int argc, char** argv) {
    const int priority = static_cast<int>(rt::arg_long(argc, argv, 1, 80, 0, 99));
    const int seconds = static_cast<int>(rt::arg_long(argc, argv, 2, 5, 1, 3600));
    const int cpu = static_cast<int>(rt::arg_long(argc, argv, 3, rt::default_cpu(), -1, CPU_SETSIZE - 1));
    const long period_us = rt::arg_long(argc, argv, 4, 10'000, 100, 1'000'000);
    const long wcet_us = rt::arg_long(argc, argv, 5, 2'000, 0, 1'000'000);

    const long long period_ns = period_us * rt::kNsPerUs;
    const long long wcet_ns = wcet_us * rt::kNsPerUs;

    if (!rt::lock_memory()) return 1;
    if (cpu >= 0 && rt::pin_self_to_cpu(cpu) != 0) return 1;
    if (rt::set_self_sched(priority) != 0) return 1;

    rt::print_self_sched("periodic_task");
    std::printf("period %ld us, computation %ld us, utilisation %.1f %%, %d s\n", period_us,
                wcet_us, 100.0 * wcet_us / period_us, seconds);

    Result r;
    timespec release = rt::now();
    rt::add_ns(release, period_ns);
    timespec end = release;
    rt::add_ns(end, static_cast<long long>(seconds) * rt::kNsPerSec);

    while (rt::diff_ns(release, end) < 0) {
        rt::sleep_until(release);
        const timespec woke = rt::now();

        rt::busy_ns(wcet_ns);  // the "control algorithm"

        const timespec done = rt::now();
        const long long response = rt::diff_ns(done, release);
        r.latency.add(rt::diff_ns(woke, release));
        r.response.add(response);

        if (response > period_ns) {
            if (r.misses < kMaxRecordedMisses) r.miss_jobs[r.misses] = r.jobs;
            ++r.misses;
        }
        ++r.jobs;

        // Next release; skip the ones that are already in the past.
        rt::add_ns(release, period_ns);
        const long long behind = rt::diff_ns(done, release);
        if (behind > 0) {
            const long long lost = behind / period_ns + 1;
            r.skipped += lost;
            rt::add_ns(release, lost * period_ns);
        }
    }

    std::printf("\njobs run %ld, releases skipped %ld\n", r.jobs, r.skipped);
    r.latency.print_us("release latency");
    r.response.print_us("response time  ");
    std::printf("deadline misses: %ld of %ld jobs (%.2f %%)\n", r.misses, r.jobs,
                r.jobs ? 100.0 * r.misses / r.jobs : 0.0);
    if (r.misses > 0) {
        std::printf("first missed jobs:");
        for (long i = 0; i < r.misses && i < kMaxRecordedMisses; ++i) {
            std::printf(" %ld", r.miss_jobs[i]);
        }
        std::printf("\n");
        std::printf("RESULT: deadline misses (worst response %lld us > period %ld us)\n",
                    r.response.max / rt::kNsPerUs, period_us);
    } else {
        std::printf("RESULT: all deadlines met (worst response %lld us, slack %lld us)\n",
                    r.response.max / rt::kNsPerUs, (period_ns - r.response.max) / rt::kNsPerUs);
    }
    return 0;
}
