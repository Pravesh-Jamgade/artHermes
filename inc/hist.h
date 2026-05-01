#ifndef HIST_H
#define HIST_H

#include <vector>
#include <unordered_map>
#include <array>
#include <string>
#include <map>
#include <deque>
#include <stdint.h>
#include <cmath>
using namespace std;

static constexpr uint64_t TAIL_START = 128;
static constexpr uint64_t TAIL_END   = 2048;   // exclusive
static constexpr uint64_t TAIL_W     = 256;
static constexpr uint64_t TAIL_BINS  = ceil((TAIL_END - TAIL_START) / TAIL_W);

struct LatencyHistogram{
    static const int kBuckets = 20;
    std::array<uint64_t, kBuckets> buckets;
    std::array<uint64_t, TAIL_BINS> tail_bins{};
    uint64_t total;
    uint64_t sum;
    uint64_t min;
    uint64_t max;
    LatencyHistogram() : total(0), sum(0), min(0), max(0) { buckets.fill(0); }
    void update(uint64_t value);
    void print(FILE *fp, const char *label, int width = 40) const;
    void printCsv(FILE *fp, const char *metric_name) const;
    void printCsvCdf(FILE *fp, const char *metric_name) const;
    double average() const;
};

#endif /* HIST_H */