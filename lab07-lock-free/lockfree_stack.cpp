// lockfree_stack.cpp - a Treiber stack: push and pop with compare-and-swap
//
// The program runs two tests:
//   1. correctness: threads push AND pop at the same time; afterwards every
//      value must have been seen exactly once (popped, or still in the stack)
//   2. throughput: the same workload on the lock-free stack and on a stack
//      protected by a mutex
//
// Memory: all nodes come from one array allocated before the threads start
// and freed after they have all been joined. A popped node is never reused
// and never freed while threads run. That is what makes this simple stack
// safe: reading old_head->next can never touch freed memory, and an address
// can never come back to the head (no ABA). aba_demo.cpp shows what goes wrong
// when nodes are reused.
//
// Usage:  ./lockfree_stack [threads] [ops_per_thread]
//   defaults: 4 threads, 200000 push+pop pairs per thread

#include "rt.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

namespace {

struct Node {
    uint64_t value;
    Node* next;
};

// -------------------------------------------------------- lock-free stack

class LockFreeStack {
public:
    void push(Node* node) {
        Node* old_head = head_.load();
        do {
            node->next = old_head;
            // If head is still old_head, make it node. Otherwise old_head is
            // updated to the current head and we try again.
        } while (!head_.compare_exchange_weak(old_head, node));
    }

    Node* pop() {
        Node* old_head = head_.load();
        while (old_head != nullptr) {
            // Safe only because nodes are never freed while threads run.
            Node* next = old_head->next;
            if (head_.compare_exchange_weak(old_head, next)) {
                return old_head;
            }
            retries_.fetch_add(1, std::memory_order_relaxed);
        }
        return nullptr;
    }

    uint64_t retries() const { return retries_.load(); }

private:
    std::atomic<Node*> head_{nullptr};
    std::atomic<uint64_t> retries_{0};  // failed CAS in pop, for curiosity
};

// ------------------------------------------------------------ mutex stack

class MutexStack {
public:
    void push(Node* node) {
        std::lock_guard<rt::Mutex> guard(mutex_);
        node->next = head_;
        head_ = node;
    }

    Node* pop() {
        std::lock_guard<rt::Mutex> guard(mutex_);
        Node* node = head_;
        if (node != nullptr) head_ = node->next;
        return node;
    }

    uint64_t retries() const { return 0; }

private:
    rt::Mutex mutex_;
    Node* head_ = nullptr;
};

// ---------------------------------------------------------------- workers

template <typename Stack>
struct Work {
    Stack* stack;
    pthread_barrier_t* start;  // all threads begin at the same moment
    Node* nodes;               // this thread's own slice of the node array
    long ops;
    std::vector<uint64_t> popped;  // values this thread popped
};

template <typename Stack>
void* worker(void* arg) {
    auto* w = static_cast<Work<Stack>*>(arg);
    pthread_barrier_wait(w->start);
    for (long i = 0; i < w->ops; ++i) {
        w->stack->push(&w->nodes[i]);
        // Pop right away: pushes and pops of all threads interleave.
        if (Node* n = w->stack->pop()) w->popped.push_back(n->value);
    }
    return nullptr;
}

struct Result {
    double seconds;
    bool correct;
    uint64_t retries;
};

template <typename Stack>
Result run(int threads, long ops) {
    Stack stack;
    std::vector<Node> nodes(static_cast<size_t>(threads) * ops);
    for (size_t i = 0; i < nodes.size(); ++i) nodes[i].value = i;

    pthread_barrier_t start;
    pthread_barrier_init(&start, nullptr, threads + 1);

    std::vector<Work<Stack>> work(threads);
    std::vector<pthread_t> ids(threads);
    for (int t = 0; t < threads; ++t) {
        work[t] = Work<Stack>{&stack, &start, &nodes[t * ops], ops, {}};
        work[t].popped.reserve(ops);
        if (rt::start_thread(&ids[t], worker<Stack>, &work[t], 0) != 0) std::exit(1);
    }

    const timespec t0 = rt::now();
    pthread_barrier_wait(&start);
    for (pthread_t id : ids) pthread_join(id, nullptr);
    const double seconds = rt::elapsed_ns(t0) / 1e9;
    pthread_barrier_destroy(&start);

    // Every value must appear exactly once: popped by some thread or left over.
    std::vector<uint8_t> seen(nodes.size(), 0);
    bool correct = true;
    auto mark = [&](uint64_t v) {
        if (v >= seen.size() || seen[v]++) correct = false;
    };
    for (auto& w : work)
        for (uint64_t v : w.popped) mark(v);
    while (Node* n = stack.pop()) mark(n->value);
    for (uint8_t s : seen)
        if (s != 1) correct = false;

    return {seconds, correct, stack.retries()};
}

}  // namespace

int main(int argc, char** argv) {
    const int threads = static_cast<int>(rt::arg_long(argc, argv, 1, 4, 1, 64));
    const long ops = rt::arg_long(argc, argv, 2, 200000, 1, 10'000'000);
    const double total_ops = 2.0 * threads * ops;  // one push + one pop

    std::printf("Treiber stack: %d threads, %ld push+pop pairs each\n", threads, ops);
    std::printf("std::atomic<Node*> is lock-free on this machine: %s\n\n",
                std::atomic<Node*>::is_always_lock_free ? "yes" : "no");

    const Result lf = run<LockFreeStack>(threads, ops);
    std::printf("lock-free  : %s, %.3f s, %.2f M ops/s, %llu CAS retries in pop\n",
                lf.correct ? "correct" : "BROKEN", lf.seconds, total_ops / lf.seconds / 1e6,
                static_cast<unsigned long long>(lf.retries));

    const Result mx = run<MutexStack>(threads, ops);
    std::printf("mutex      : %s, %.3f s, %.2f M ops/s\n", mx.correct ? "correct" : "BROKEN",
                mx.seconds, total_ops / mx.seconds / 1e6);

    std::printf("\nfaster here: %s (%.1fx)\n", lf.seconds < mx.seconds ? "lock-free" : "mutex",
                lf.seconds < mx.seconds ? mx.seconds / lf.seconds : lf.seconds / mx.seconds);
    std::printf("Throughput is an average. Neither number says anything about the worst case.\n");

    return lf.correct && mx.correct ? 0 : 1;
}
