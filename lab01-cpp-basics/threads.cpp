// threads.cpp - creating threads, passing data in, getting results out
//
// The program starts three workers two ways:
//   1. with pthread_create, the API every real-time lab uses, because only
//      pthreads let you set a scheduling policy before the thread starts
//   2. with std::thread, the everyday C++ way, for comparison
//
// Usage:  ./threads

#include <pthread.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// Everything a worker needs, and room for its result. Each thread gets its
// own struct, so no two threads write the same memory and no lock is needed.
struct Work {
    int id = 0;
    long amount = 0;   // input
    long long sum = 0; // output
};

void compute(Work& w) {
    long long sum = 0;
    for (long i = 0; i < w.amount; ++i) sum += i;
    w.sum = sum;
}

// pthread entry points must have exactly this signature.
void* worker(void* arg) {
    Work* w = static_cast<Work*>(arg);  // the pointer we passed to pthread_create
    compute(*w);
    return nullptr;
}

}  // namespace

int main() {
    constexpr int kThreads = 3;

    // 1. pthreads
    std::printf("1. pthread_create / pthread_join\n");
    pthread_t tids[kThreads];
    Work work[kThreads];
    for (int i = 0; i < kThreads; ++i) {
        work[i].id = i;
        work[i].amount = (i + 1) * 1'000'000L;
        // pthread functions return an error number; they do not set errno.
        int rc = pthread_create(&tids[i], nullptr, worker, &work[i]);
        if (rc != 0) {
            std::fprintf(stderr, "pthread_create: %s\n", std::strerror(rc));
            return 1;
        }
    }
    for (int i = 0; i < kThreads; ++i) {
        int rc = pthread_join(tids[i], nullptr);  // wait for the thread to finish
        if (rc != 0) {
            std::fprintf(stderr, "pthread_join: %s\n", std::strerror(rc));
            return 1;
        }
    }
    for (const Work& w : work) {
        std::printf("   thread %d: sum of 0..%ld = %lld\n", w.id, w.amount - 1, w.sum);
    }

    // 2. std::thread does the same with less ceremony, but it has no way to
    //    pass scheduling attributes. Fine for ordinary code, not for RT tasks.
    std::printf("\n2. std::thread\n");
    std::vector<Work> more(kThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        more[i].id = i;
        more[i].amount = (i + 1) * 2'000'000L;
        threads.emplace_back([&w = more[i]] { compute(w); });
    }
    for (auto& t : threads) t.join();
    for (const Work& w : more) {
        std::printf("   thread %d: sum of 0..%ld = %lld\n", w.id, w.amount - 1, w.sum);
    }
    return 0;
}
