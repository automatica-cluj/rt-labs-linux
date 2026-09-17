// policy_mix.cpp - SCHED_OTHER, SCHED_RR and SCHED_FIFO competing for one CPU
//
// The same amount of work under three policies, all pinned to one CPU:
//   O  SCHED_OTHER
//   R  SCHED_RR   at `priority`
//   F  SCHED_FIFO at `priority`   (same priority as R)
// They are released in the order O, R, F.
//
// Things to look for in the output:
//   - O does (almost) nothing while a real-time task is runnable.
//   - R and F have the same priority. R was queued first, so it starts, but
//     after one RR time slice it goes to the back of the queue, behind F. F is
//     FIFO: it is never sliced and keeps the CPU until it is done.
//   - Since Linux 6.12 the "fair server" (a SCHED_DEADLINE reservation) still
//     gives normal tasks up to 50 ms per second, so O may get a few pieces in.
//
// Usage:  ./policy_mix [priority] [cpu] [work_ms]
//   priority  1..79 for R and F. Default 50.
//   cpu       CPU to pin to, -1 for no pinning. Default: derived from your user id.
//   work_ms   CPU time per worker, up to 600. Default 150.

#include "timeline.hpp"

int main(int argc, char** argv) {
    const int prio = static_cast<int>(rt::arg_long(argc, argv, 1, 50, 1, 79));
    const int cpu = static_cast<int>(rt::arg_long(argc, argv, 2, rt::default_cpu(), -1, CPU_SETSIZE - 1));
    const long work_ms = rt::arg_long(argc, argv, 3, 150, 1, 600);

    lab::setup_main(prio + 1, cpu);

    std::vector<lab::Worker> w(3);
    w[0].name = "other";
    w[0].tag = 'O';
    w[0].policy = SCHED_OTHER;
    w[0].priority = 0;
    w[1].name = "rr";
    w[1].tag = 'R';
    w[1].policy = SCHED_RR;
    w[1].priority = prio;
    w[2].name = "fifo";
    w[2].tag = 'F';
    w[2].policy = SCHED_FIFO;
    w[2].priority = prio;
    for (auto& x : w) {
        x.cpu = cpu;
        x.work_ns = work_ms * rt::kNsPerMs;
    }

    std::printf("3 workers x %ld ms of CPU time on %s, released in order O R F\n", work_ms,
                lab::cpu_text(cpu));
    lab::start_all(w);
    lab::report(w);

    const bool other_last = w[0].finish_ns > w[1].finish_ns && w[0].finish_ns > w[2].finish_ns;
    std::printf("result: SCHED_OTHER finished %s\n",
                other_last ? "last, after both real-time workers" : "BEFORE a real-time worker");
    return 0;
}
