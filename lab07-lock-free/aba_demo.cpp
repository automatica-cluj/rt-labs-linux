// aba_demo.cpp - the ABA problem, step by step, and the tagged-pointer fix
//
// Two threads share a lock-free stack  A -> B -> C.  The interleaving is
// forced with a step counter, so the bad case happens on every run:
//
//   thread 1: starts pop, reads head = A and A->next = B, then is "preempted"
//   thread 2: pops A, pops B, frees B, reuses node A for a new value, pushes A
//             stack is now  A' -> C   (A' is the same address as A)
//   thread 1: resumes, CAS(head, A, B) ...
//
// A plain CAS compares addresses only. The head is A again, so the CAS
// succeeds and installs B, a node that was already freed. That is ABA: the
// value went A -> B -> A and the CAS could not tell.
//
// The fix used here: the head is a (pointer, tag) pair and every successful
// push or pop increments the tag. Thread 1 read (A, tag 3); after thread 2's
// two pops and one push the head is (A, tag 6), so the CAS fails and thread 1
// retries with the real head.
//
// Usage:  ./aba_demo            reused nodes go to a free list (no real free)
//         ./aba_demo delete     really delete node B (only in the ASan build:
//                               make asan && ./build/aba_demo_asan delete)

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

enum class State { kInStack, kPopped, kFree };

struct Node {
    char name;
    int value;
    State state;
    Node* next;
};

bool g_real_delete = false;

// "Free" a node: in normal mode it is only marked, so the program can report
// what happened. In delete mode the memory really goes back to the allocator.
void free_node(Node* n) {
    if (g_real_delete) {
        delete n;
    } else {
        n->state = State::kFree;
    }
}

// ----------------------------------------------------- forced interleaving

class Steps {
public:
    void wait_for(int step) {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return step_ == step; });
    }
    void go_to(int step) {
        {
            std::lock_guard<std::mutex> lock(m_);
            step_ = step;
        }
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    int step_ = 0;
};

// ----------------------------------------------------------------- stacks

// Plain Treiber stack: the head is just a pointer.
class PlainStack {
public:
    struct Snapshot {
        Node* head;
    };
    static const char* name() { return "plain pointer CAS"; }

    void push(Node* n) {
        Node* old = head_.load();
        do {
            n->next = old;
        } while (!head_.compare_exchange_weak(old, n));
        n->state = State::kInStack;
    }
    Node* pop() {
        Snapshot s = read();
        while (s.head && !try_pop(s, s.head->next)) {
        }
        return s.head;
    }
    Snapshot read() const { return {head_.load()}; }
    std::string describe(const Snapshot& s) const { return std::string(1, s.head->name); }

    // The CAS at the heart of pop. On failure s is refreshed to the current head.
    bool try_pop(Snapshot& s, Node* next) {
        if (head_.compare_exchange_strong(s.head, next)) {
            s.head->state = State::kPopped;
            return true;
        }
        return false;
    }
    Node* top() const { return head_.load(); }

private:
    std::atomic<Node*> head_{nullptr};
};

// Tagged stack: the head is (pointer, tag); the tag changes on every update.
class TaggedStack {
public:
    struct Snapshot {
        Node* head;
        uint64_t tag;
    };
    static const char* name() { return "tagged pointer CAS"; }

    void push(Node* n) {
        Snapshot old = head_.load();
        do {
            n->next = old.head;
        } while (!head_.compare_exchange_weak(old, Snapshot{n, old.tag + 1}));
        n->state = State::kInStack;
    }
    Node* pop() {
        Snapshot s = read();
        while (s.head && !try_pop(s, s.head->next)) {
        }
        return s.head;
    }
    Snapshot read() const { return head_.load(); }
    std::string describe(const Snapshot& s) const {
        return std::string(1, s.head->name) + " tag " + std::to_string(s.tag);
    }
    bool try_pop(Snapshot& s, Node* next) {
        if (head_.compare_exchange_strong(s, Snapshot{next, s.tag + 1})) {
            s.head->state = State::kPopped;
            return true;
        }
        return false;
    }
    Node* top() const { return head_.load().head; }

private:
    // Two 64-bit words and no padding, so the CAS compares exactly pointer+tag.
    // A double-width CAS: see the README for -mcx16 and libatomic.
    std::atomic<Snapshot> head_{Snapshot{nullptr, 0}};
};

// --------------------------------------------------------------- scenario

template <typename Stack>
bool scenario() {
    std::printf("=== %s ===\n", Stack::name());

    Stack stack;
    Node* a = new Node{'A', 1, State::kFree, nullptr};
    Node* b = new Node{'B', 2, State::kFree, nullptr};
    Node* c = new Node{'C', 3, State::kFree, nullptr};
    stack.push(c);
    stack.push(b);
    stack.push(a);
    std::printf("initial stack: A(1) -> B(2) -> C(3)\n");

    Steps steps;
    Node* t1_popped = nullptr;
    bool t1_first_cas_ok = false;

    std::thread t1([&] {
        typename Stack::Snapshot s = stack.read();
        Node* next = s.head->next;
        const char next_name = next->name;  // remembered, B may be freed later
        std::printf("thread 1: read head %s, next %c, then gets preempted\n",
                    stack.describe(s).c_str(), next_name);
        steps.go_to(1);
        steps.wait_for(2);

        t1_first_cas_ok = stack.try_pop(s, next);
        if (t1_first_cas_ok) {
            std::printf("thread 1: CAS(head, A -> %c) succeeded\n", next_name);
        } else {
            std::printf("thread 1: CAS failed, head is now %s. Retrying.\n",
                        stack.describe(s).c_str());
            while (!stack.try_pop(s, s.head->next)) {
            }
        }
        t1_popped = s.head;
    });

    std::thread t2([&] {
        steps.wait_for(1);
        Node* x = stack.pop();
        Node* y = stack.pop();
        std::printf("thread 2: popped %c(%d) and %c(%d), frees %c\n", x->name, x->value, y->name,
                    y->value, y->name);
        free_node(y);
        x->value = 4;  // node A is recycled for a new value
        stack.push(x);
        std::printf("thread 2: pushed recycled node A(4). Stack: A(4) -> C(3)\n");
        steps.go_to(2);
    });

    t1.join();
    t2.join();

    // Values 1, 2, 3 and 4 were pushed. 1 and 2 were popped by thread 2,
    // thread 1 popped one more, so exactly one value must be left.
    std::printf("thread 1 popped value %d\n", t1_popped->value);
    std::printf("stack now:");
    int left = 0;
    bool freed_node_reachable = false;
    for (Node* n = stack.top(); n != nullptr && left < 10; n = n->next, ++left) {
        const bool freed = n->state == State::kFree;
        freed_node_reachable |= freed;
        std::printf(" %c(%d)%s ->", n->name, n->value, freed ? " [FREED]" : "");
    }
    std::printf(" end\n");

    const bool aba = t1_first_cas_ok && (freed_node_reachable || left != 1);
    if (aba) {
        std::printf("ABA happened: the head was A again, so the stale CAS succeeded.\n");
        std::printf("  The stack now starts at freed node B, value 2 is 'in' the stack a\n");
        std::printf("  second time and the %d left entries should have been 1.\n\n", left);
    } else if (!t1_first_cas_ok) {
        std::printf("tagged CAS rejected stale head: the tag changed while thread 1 was away.\n");
        std::printf("  The retry popped the real head, %d entry left, no freed node reachable.\n\n",
                    left);
    }

    // In delete mode B is already gone; the walk above read freed memory.
    delete a;
    if (!g_real_delete) delete b;
    delete c;
    return aba;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "delete") == 0) {
#if defined(__SANITIZE_ADDRESS__)
        g_real_delete = true;
        std::printf("delete mode: node B is really freed. AddressSanitizer will report\n"
                    "the read of freed memory below.\n\n");
        scenario<PlainStack>();
        return 0;
#else
        std::fprintf(stderr, "delete mode reads freed memory (undefined behaviour).\n"
                             "Run it only in the ASan build: make asan && ./build/aba_demo_asan delete\n");
        return 2;
#endif
    }

    std::printf("std::atomic<(pointer, tag)> is lock-free here: %s\n\n",
                std::atomic<TaggedStack::Snapshot>{}.is_lock_free() ? "yes" : "no (uses a lock)");

    const bool plain_aba = scenario<PlainStack>();
    const bool tagged_aba = scenario<TaggedStack>();

    // Expected: ABA with the plain stack, no ABA with the tagged one.
    return plain_aba && !tagged_aba ? 0 : 1;
}
