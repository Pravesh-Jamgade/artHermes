#include "llc_pred_perc.h"
#include "champsim.h"
#include "defs.h"
#include <cstdio>

// Deadblock pred
PereceptronDeadblockPredictor::PereceptronDeadblockPredictor()
{
    for (int f = 0; f < NUM_FEATURES; f++)
        for (int i = 0; i < WT_SIZE; i++)
            weights[f][i] = 0;
    bzero(&stats, sizeof(stats));
}

// Deadblock pred
PereceptronDeadblockPredictor::~PereceptronDeadblockPredictor() {}

// Deadblock pred
uint64_t PereceptronDeadblockPredictor::get_first_touch_mask(uint64_t ppn) const
{
    auto it = page_first_touch.find(ppn);
    return (it != page_first_touch.end()) ? it->second : 0;
}

// Deadblock pred
void PereceptronDeadblockPredictor::compute_features(uint64_t full_addr, uint64_t ip, uint32_t *features) const
{
    uint64_t block_addr = full_addr >> LOG2_BLOCK_SIZE;
    uint64_t ppn = block_addr >> (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE);
    uint64_t cl_offset = block_addr & ((1ull << (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE)) - 1);

    features[0] = (uint32_t)(block_addr % WT_SIZE);
    features[1] = (uint32_t)(cl_offset % WT_SIZE);
    features[2] = (uint32_t)(ppn % WT_SIZE);
    features[3] = (uint32_t)((ip >> 2) % WT_SIZE);
    features[4] = (uint32_t)((ppn ^ (ip >> 2)) % WT_SIZE);

    // First-touch bitmask feature: hash of 64-bit per-page access pattern
    uint64_t ft_mask = get_first_touch_mask(ppn);
    uint64_t ft_hash = ft_mask;
    ft_hash ^= ft_mask >> 32;
    ft_hash ^= ft_mask >> 16;
    ft_hash ^= ft_mask >> 8;
    features[5] = (uint32_t)(ft_hash % WT_SIZE);
}

// Deadblock pred
bool PereceptronDeadblockPredictor::predict(uint64_t full_addr, uint64_t ip)
{
    uint32_t features[NUM_FEATURES];
    compute_features(full_addr, ip, features);

    int32_t sum = 0;
    for (int f = 0; f < NUM_FEATURES; f++)
        sum += weights[f][features[f]];

    // Set first-touch bit for this cache block within its 4KB page
    update_first_touch(full_addr);
    return sum >= 0;
}

// Deadblock pred
void PereceptronDeadblockPredictor::train(uint64_t full_addr, uint64_t ip, bool predicted_doa, bool actual_doa)
{
    if (predicted_doa == actual_doa)
        return; // correct, no weight update

    uint32_t features[NUM_FEATURES];
    compute_features(full_addr, ip, features);

    int delta = actual_doa ? 1 : -1;
    for (int f = 0; f < NUM_FEATURES; f++) {
        int16_t &w = weights[f][features[f]];
        if (delta > 0 && w < 15)
            w++;
        else if (delta < 0 && w > -16)
            w--;
    }
}

// Deadblock pred
void PereceptronDeadblockPredictor::update_first_touch(uint64_t full_addr)
{
    uint64_t block_addr = full_addr >> LOG2_BLOCK_SIZE;
    uint64_t ppn = block_addr >> (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE);
    uint64_t cl_index = block_addr & ((1ull << (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE)) - 1);
    uint64_t &mask = page_first_touch[ppn];
    if (mask == 0) stats.pages_tracked++;
    mask |= (1ull << cl_index);
}

// Deadblock pred
void PereceptronDeadblockPredictor::record_eviction(bool pred_doa, bool actual_doa)
{
    stats.evictions++;
    if (pred_doa == actual_doa) {
        stats.eviction_correct++;
        if (pred_doa)  stats.eviction_tp++;
        else                    stats.eviction_tn++;
    } else {
        if (pred_doa)  stats.eviction_fp++;
        else                    stats.eviction_fn++;
    }
}

// Deadblock pred
void PereceptronDeadblockPredictor::dump_stats(FILE *stream, const char *prefix)
{
    uint64_t ev_total = stats.evictions;
    float ev_acc = ev_total ? (100.0 * stats.eviction_correct / ev_total) : 0;
    float ev_prec = (stats.eviction_tp + stats.eviction_fp) ? (100.0 * stats.eviction_tp / (stats.eviction_tp + stats.eviction_fp)) : 0;
    float ev_rec  = (stats.eviction_tp + stats.eviction_fn) ? (100.0 * stats.eviction_tp / (stats.eviction_tp + stats.eviction_fn)) : 0;

    fprintf(stream, "%s_eviction_total %lu\n", prefix, ev_total);
    fprintf(stream, "%s_eviction_correct %lu\n", prefix, stats.eviction_correct);
    fprintf(stream, "%s_eviction_accuracy %.2f\n", prefix, ev_acc);
    fprintf(stream, "%s_eviction_precision %.2f\n", prefix, ev_prec);
    fprintf(stream, "%s_eviction_recall %.2f\n", prefix, ev_rec);
    fprintf(stream, "%s_eviction_tp %lu\n", prefix, stats.eviction_tp);
    fprintf(stream, "%s_eviction_fp %lu\n", prefix, stats.eviction_fp);
    fprintf(stream, "%s_eviction_tn %lu\n", prefix, stats.eviction_tn);
    fprintf(stream, "%s_eviction_fn %lu\n", prefix, stats.eviction_fn);

    fprintf(stream, "%s_pages_tracked %lu\n", prefix, stats.pages_tracked);
}

// Deadblock pred
void PereceptronDeadblockPredictor::reset_stats()
{
    for (int f = 0; f < NUM_FEATURES; f++)
        for (int i = 0; i < WT_SIZE; i++)
            weights[f][i] = 0;
    page_first_touch.clear();
    bzero(&stats, sizeof(stats));
}
