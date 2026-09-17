// fifo_order.cpp - strict priority order under SCHED_FIFO
//
// Three CPU-bound workers, High, Medium and Low, all pinned to one CPU. They
// are released by main in the order Low, Medium, High. Main runs one priority
// step above them, so none can start before main blocks in pthread_join. At
// that moment all three are runnable and the scheduler picks.
//
//   SCHED_FIFO: the highest priority runs to completion, then the next one.
//               Each worker runs in one piece.
//   SCHED_OTHER (priority 0): the fair scheduler interleaves all three.
//
// Usage:  ./fifo_order [priority] [cpu] [work_ms]
//   priority  priority of High (21..79); Medium = priority-10, Low = priority-20.
//             0 -> everything SCHED_OTHER, for comparison. Default 70.
//   cpu       CPU to pin to, -1 for no pinning. Default: derived from your user id.
//   work_ms   CPU time each worker burns. Default 150.

#include "timeline.hpp"

int main(int argc, char** argv) {
    long top = rt::arg_long(argc, argv, 1, 70, 0, 79);
    if (top != 0 && top < 21) {
        std::fprintf(stderr, "priority must be 0 or in [21, 79]\n");
        return 2;
    }
    const int cpu = static_cast<int>(rt::arg_long(argc, argv, 2, rt::default_cpu(), -1, CPU_SETSIZE - 1));
    const long work_ms = rt::arg_long(argc, argv, 3, 150, 1, 300);

    const int prio = static_cast<int>(top);
    const int policy = prio > 0 ? SCHED_FIFO : SCHED_OTHER;

    // Main sits one step above High so the workers wait for it.
    lab::setup_main(prio > 0 ? prio + 1 : 0, cpu);

    std::vector<lab::Worker> w(3);
    // Released lowest priority first: if release order decided anything,
    // Low would win. Under SCHED_FIFO priority wins instead.
    const char* names[] = {"Low", "Medium", "High"};
    const char tags[] = {'L', 'M', 'H'};
    for (int i = 0; i < 3; ++i) {
        w[i].name = names[i];
        w[i].tag = tags[i];
        w[i].policy = policy;
        w[i].priority = prio > 0 ? prio - 20 + 10 * i : 0;
        w[i].cpu = cpu;
        w[i].work_ns = work_ms * rt::kNsPerMs;
    }

    std::printf("3 workers x %ld ms of CPU time on %s, released in order L M H\n", work_ms,
                lab::cpu_text(cpu));
    lab::start_all(w);
    lab::report(w);

    if (prio > 0) {
        const bool ordered = lab::order_by(w, &lab::Worker::finish_ns) == "H M L";
        std::printf("result: %s, %s\n",
                    ordered ? "finished in priority order" : "NOT in priority order",
                    lab::parallel(w)      ? "workers ran in parallel"
                    : lab::interleaved(w) ? "workers took turns"
                                          : "one after another");
    } else {
        std::printf("result: SCHED_OTHER, the fair scheduler shared the CPU\n");
    }
    return 0;
}
