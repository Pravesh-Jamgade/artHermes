#ifndef LLC_PRED_PERC_H
#define LLC_PRED_PERC_H

#include <cstdint>
#include <cstdio>
#include <unordered_map>

// Deadblock pred: Perceptron-based LLC read hit/miss predictor keyed on physical address.
// Uses 6 features derived from physical address and instruction PC:
//   0: Physical block address hash
//   1: Cache-line offset within physical page
//   2: Physical page number hash  
//   3: Instruction PC hash
//   4: PC XOR physical page
//   5: First-touch bitmask hash (64-bit per-page bitmask, 1 bit per 64B block in 4KB page)
// Each feature has a 1024-entry int16_t weight table.
class PereceptronDeadblockPredictor {
public:
    static const int NUM_FEATURES = 6;
    static const int WT_SIZE = 1024;

    struct {

        // Lifetime prediction stats (DOA vs reused at eviction)
        uint64_t evictions;
        uint64_t eviction_correct;
        uint64_t eviction_tp; // pred non-DOA, actually reused
        uint64_t eviction_fp; // pred non-DOA, but DOA
        uint64_t eviction_tn; // pred DOA,     actually DOA
        uint64_t eviction_fn; // pred DOA,     but reused

        // First-touch coverage
        uint64_t pages_tracked;
    } stats;

    PereceptronDeadblockPredictor();
    ~PereceptronDeadblockPredictor();

    // Returns true if predicted READ HIT, false if predicted READ MISS
    bool predict(uint64_t full_addr, uint64_t ip);

    // Train predictor with actual outcome (true = hit, false = miss).
    // Also records instant prediction stats.
    void train(uint64_t full_addr, uint64_t ip, bool predicted_hit, bool actual_hit);

    // Record eviction-time prediction (doa_pred_bit vs usage).
    void record_eviction(bool predicted_non_doa, bool actual_non_doa);

    void dump_stats(FILE *stream, const char *prefix);
    void reset_stats();

private:
    int16_t weights[NUM_FEATURES][WT_SIZE];
    std::unordered_map<uint64_t, uint64_t> page_first_touch;

    void compute_features(uint64_t full_addr, uint64_t ip, uint32_t *features) const;
    uint64_t get_first_touch_mask(uint64_t ppn) const;
    void update_first_touch(uint64_t full_addr);
};

#endif
