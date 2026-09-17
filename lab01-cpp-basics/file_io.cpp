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

#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

const int kCount = 10;

struct Measurement {
    int index;
    double at_ms;        // time since the start
    double interval_ms;  // time since the previous measurement
};

double now_ms() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "measurements.txt";

    // 1. Measure. The array is allocated before the loop, so the loop itself
    //    only sleeps, reads the clock and stores a value.
    Measurement data[kCount];
    std::srand(1);
    const double start = now_ms();
    double prev = start;
    for (int i = 0; i < kCount; ++i) {
        const long ms = 10 + std::rand() % 91;  // 10..100 ms
        timespec req = {0, ms * 1000000L};
        nanosleep(&req, nullptr);
        const double t = now_ms();
        data[i].index = i;
        data[i].at_ms = t - start;
        data[i].interval_ms = t - prev;
        prev = t;
    }

    // 2. Write. The header lines start with '#' so plotting tools skip them.
    std::ofstream out(path);
    if (!out) {
        std::perror(path);
        return 1;
    }
    out << "# index at_ms interval_ms\n";
    for (int i = 0; i < kCount; ++i) {
        out << data[i].index << ' ' << data[i].at_ms << ' ' << data[i].interval_ms << '\n';
    }
    out.close();
    if (!out) {
        std::fprintf(stderr, "error while writing %s\n", path);
        return 1;
    }
    std::printf("wrote %d measurements to %s\n", kCount, path);

    // 3. Summarise from the stored data.
    double min = data[0].interval_ms;
    double max = data[0].interval_ms;
    double sum = 0;
    for (int i = 0; i < kCount; ++i) {
        if (data[i].interval_ms < min) min = data[i].interval_ms;
        if (data[i].interval_ms > max) max = data[i].interval_ms;
        sum += data[i].interval_ms;
    }
    std::printf("interval: min %.2f ms, avg %.2f ms, max %.2f ms\n", min, sum / kCount, max);
    return 0;
}
