// same_priority.cpp - FIFO versus RR versus OTHER when priorities are equal
//
// Three CPU-bound workers A, B, C with the SAME policy and priority, pinned to
// one CPU and released in the order A, B, C (see timeline.hpp).
//
//   fifo   the first one runs to completion, then the next
//   rr     each runs for one time slice (usually 100 ms, see
//          /proc/sys/kernel/sched_rr_timeslice_ms), then goes to the back
//          of the queue
//   other  the fair scheduler switches every few milliseconds
//
// Usage:  ./same_priority [fifo|rr|other] [priority] [cpu] [work_ms]
//   priority  1..79 for fifo/rr, ignored for other. Default 50.
//   cpu       CPU to pin to, -1 for no pinning. Default: derived from your user id.
//   work_ms   CPU time per worker. Default 150.

#include "timeline.hpp"

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "fifo";
    int policy;
    if (mode == "fifo") policy = SCHED_FIFO;
    else if (mode == "rr") policy = SCHED_RR;
    else if (mode == "other") policy = SCHED_OTHER;
    else {
        std::fprintf(stderr, "usage: %s [fifo|rr|other] [priority] [cpu] [work_ms]\n", argv[0]);
        return 2;
    }
    int prio = static_cast<int>(rt::arg_long(argc, argv, 2, 50, 1, 79));
    if (policy == SCHED_OTHER) prio = 0;
    const int cpu = static_cast<int>(rt::arg_long(argc, argv, 3, rt::default_cpu(), -1, CPU_SETSIZE - 1));
    const long work_ms = rt::arg_long(argc, argv, 4, 150, 1, 300);

    lab::setup_main(prio > 0 ? prio + 1 : 0, cpu);

    if (policy == SCHED_RR) {
        if (FILE* f = std::fopen("/proc/sys/kernel/sched_rr_timeslice_ms", "r")) {
            int ms = 0;
            if (std::fscanf(f, "%d", &ms) == 1) std::printf("SCHED_RR time slice: %d ms\n", ms);
            std::fclose(f);
        }
    }

    std::vector<lab::Worker> w(3);
    const char* names[] = {"A", "B", "C"};
    for (int i = 0; i < 3; ++i) {
        w[i].name = names[i];
        w[i].tag = names[i][0];
        w[i].policy = policy;
        w[i].priority = prio;
        w[i].cpu = cpu;
        w[i].work_ns = work_ms * rt::kNsPerMs;
    }

    std::printf("3 workers x %ld ms of CPU time, %s priority %d, %s\n", work_ms,
                rt::policy_name(policy), prio, lab::cpu_text(cpu));
    lab::start_all(w);
    lab::report(w);

    std::printf("result: %s\n", lab::parallel(w)      ? "workers ran in parallel"
                                 : lab::interleaved(w) ? "workers took turns (time slicing)"
                                                       : "one after another, each ran to completion");
    return 0;
}
