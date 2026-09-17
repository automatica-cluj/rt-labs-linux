// file_io.cpp - store measurements in structs, write them to a data file
//
// The program:
//   1. takes 10 timestamps a random 10..100 ms apart and stores them
//   2. writes them to a text file that Python or gnuplot can read
//   3. computes min / avg / max from the stored data
//
// Usage:  ./file_io [output-file]      (default: measurements.txt)
//
// Collect first, write afterwards. Writing to a file inside a timing loop
// adds unpredictable delays to exactly what you are trying to measure.

#include <time.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>
#include <vector>

namespace {

struct Measurement {
    int index = 0;
    double at_ms = 0;        // time since the start
    double interval_ms = 0;  // time since the previous measurement
};

double now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "measurements.txt";
    constexpr int kCount = 10;

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<long> sleep_ms(10, 100);

    // 1. Measure. reserve() allocates once, before the loop.
    std::vector<Measurement> data;
    data.reserve(kCount);
    const double start = now_ms();
    double prev = start;
    for (int i = 0; i < kCount; ++i) {
        const long ms = sleep_ms(rng);
        timespec req{0, ms * 1'000'000L};
        nanosleep(&req, nullptr);
        const double t = now_ms();
        data.push_back({i, t - start, t - prev});
        prev = t;
    }

    // 2. Write. The header lines start with '#' so plotting tools skip them.
    std::ofstream out(path);
    if (!out) {
        std::perror(path);
        return 1;
    }
    out << "# index at_ms interval_ms\n";
    for (const Measurement& m : data) {
        out << m.index << ' ' << m.at_ms << ' ' << m.interval_ms << '\n';
    }
    out.close();
    if (!out) {
        std::fprintf(stderr, "error while writing %s\n", path);
        return 1;
    }
    std::printf("wrote %d measurements to %s\n", kCount, path);

    // 3. Summarise from the stored data.
    const auto [lo, hi] = std::minmax_element(data.begin(), data.end(),
        [](const Measurement& a, const Measurement& b) { return a.interval_ms < b.interval_ms; });
    double sum = 0;
    for (const Measurement& m : data) sum += m.interval_ms;
    std::printf("interval: min %.2f ms, avg %.2f ms, max %.2f ms\n", lo->interval_ms,
                sum / kCount, hi->interval_ms);
    return 0;
}
