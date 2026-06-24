#include "hist.h"
#include <inttypes.h>
#include <sstream>
#include <cmath>

void LatencyHistogram::update(uint64_t value){
    uint64_t bucket = 0;
    uint64_t shifted = value;
    while (shifted > 1 && bucket + 1 < kBuckets)
    {
        shifted >>= 1;
        bucket++;
    }
    buckets[bucket]++;
    total++;
    sum += value;
    if(total == 1){
        min = value;
        max = value;
    } else {
        min = std::min(min, value);
        max = std::max(max, value);
    }

        // tail zoom bins
    int tail_size = std::ceil((value - TAIL_START) / static_cast<double>(TAIL_W));
    if (value >= TAIL_START && value < TAIL_END) {
        tail_bins[tail_size]++;
    }
}

double LatencyHistogram::average() const{
    return total ? static_cast<double>(sum) / static_cast<double>(total) : 0.0;
}

void LatencyHistogram::print(FILE *fp, const char *label, int width) const{
        if(!fp)
            return;
        uint64_t max_count = 0;
        for (uint64_t count : buckets)
            max_count = std::max(max_count, count);
        fprintf(fp, "%s (total=%" PRIu64 ", min=%" PRIu64 ", max=%" PRIu64 ", avg=%.2f)\n", label, total, min, max, average());
        if(max_count == 0)
            return;
        for (int idx = 0; idx < kBuckets; ++idx){
            uint64_t count = buckets[idx];
            if(!count)
                continue;
            uint64_t range_start = (idx == 0) ? 0 : (uint64_t(1) << idx);
            uint64_t range_end = (idx + 1 < kBuckets) ? ((uint64_t(1) << (idx + 1)) - 1) : 0;
            int bar_len = static_cast<int>(count * width / max_count);
            fprintf(fp, "  [%8" PRIu64 "..", range_start);
            if(idx + 1 < kBuckets)
                fprintf(fp, "%8" PRIu64 "] ", range_end);
            else
                fprintf(fp, "    inf] ");
            for (int c = 0; c < bar_len; ++c)
                fputc('#', fp);
            fprintf(fp, " (%" PRIu64 ")\n", count);
        }
    }


    void LatencyHistogram::printCsv(FILE *fp, const char *metric_name) const{
        if(!fp)
            return;
        fprintf(fp, "%s_avg,%.2f\n", metric_name, average());
        std::vector<std::pair<std::string, uint64_t>> entries;
        entries.reserve(kBuckets);
        for (int idx = 0; idx < kBuckets; ++idx){
            uint64_t count = buckets[idx];
            if(!count)
                continue;
            uint64_t range_start = (idx == 0) ? 0 : (uint64_t(1) << idx);
            std::ostringstream label;
            label << "[" << range_start << "..";
            if(idx + 1 < kBuckets){
                uint64_t range_end = (uint64_t(1) << (idx + 1)) - 1;
                label << range_end << "]";
            } else {
                label << "inf]";
            }
            entries.emplace_back(label.str(), count);
        }
        if(entries.empty())
            return;
        fprintf(fp, "%s", metric_name);
        for (const auto &entry : entries)
            fprintf(fp, ",%s", entry.first.c_str());
        fprintf(fp, "\n");
        fprintf(fp, "%s", metric_name);
        for (const auto &entry : entries)
            fprintf(fp, ",%" PRIu64, entry.second);
        fprintf(fp, "\n");

        // tail latencies
        entries.clear();
        for (size_t i = 0; i < TAIL_BINS; ++i) {
            uint64_t count = tail_bins[i];
            uint64_t range_start = TAIL_START + i * TAIL_W;
            uint64_t range_end = range_start + TAIL_W - 1;
            std::ostringstream label;
            label << "[" << range_start << ".." << range_end << "]";
            entries.emplace_back(label.str(), count);
        }

        fprintf(fp, "%s_tail", metric_name);
        for (const auto &entry : entries)
            fprintf(fp, ",%s", entry.first.c_str());
        fprintf(fp, "\n");
        fprintf(fp, "%s_tail", metric_name);
        for (const auto &entry : entries)
            fprintf(fp, ",%" PRIu64, entry.second);
        fprintf(fp, "\n"); 
    }

    void LatencyHistogram::printCsvCdf(FILE *fp, const char *metric_name) const{
        if(!fp || total == 0)
            return;
        printCsv(fp, metric_name);
        std::vector<std::pair<std::string, uint64_t>> entries;
        entries.reserve(kBuckets);
        for (int idx = 0; idx < kBuckets; ++idx){
            uint64_t count = buckets[idx];
            if(!count)
                continue;
            uint64_t range_start = (idx == 0) ? 0 : (uint64_t(1) << idx);
            std::ostringstream label;
            label << "[" << range_start << "..";
            if(idx + 1 < kBuckets){
                uint64_t range_end = (uint64_t(1) << (idx + 1)) - 1;
                label << range_end << "]";
            } else {
                label << "inf]";
            }
            entries.emplace_back(label.str(), count);
        }
        if(entries.empty())
            return;
        fprintf(fp, "%s_cdf", metric_name);
        for (const auto &entry : entries)
            fprintf(fp, ",%s", entry.first.c_str());
        fprintf(fp, "\n");
        fprintf(fp, "%s_cdf", metric_name);
        uint64_t cumulative = 0;
        for (const auto &entry : entries){
            cumulative += entry.second;
            double pct = static_cast<double>(cumulative) / static_cast<double>(total) * 100.0;
            fprintf(fp, ",%.2f", pct);
        }
        fprintf(fp, "\n");
    }
