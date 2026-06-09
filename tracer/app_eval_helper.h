// tlb_3c.cpp
// A simple set-associative TLB with LRU replacement + 3C miss classification:
//   - Compulsory: first time this VPN is ever referenced
//   - Conflict  : miss in set-assoc TLB, but HIT in a fully-associative shadow TLB of same total entries
//   - Capacity  : miss in both (set-assoc + fully-assoc), but VPN has been seen before
//
// Build: g++ -O2 -std=c++17 tlb_3c.cpp -o tlb_3c
// Use:   include the class in your sim, call access(vaddr, ...), then print stats.

#include <cstdint>      // uint64_t
#include <vector>       // std::vector
#include <unordered_set>// std::unordered_set
#include <iostream>     // std::cout
#include <iomanip>      // std::setprecision
#include <optional>     // std::optional
#include <cassert>      // assert
#include <sstream>
// -------------------------------
// A tiny helper to compute log2 for powers of two.
// -------------------------------
static inline unsigned ilog2_pow2(uint64_t x) {
    // We assume x is a power of two.
    assert(x && (x & (x - 1)) == 0);
    unsigned r = 0;
    while (x >>= 1) r++;
    return r;
}

// -------------------------------
// TLB entry structure (stores VPN tag + optional PPN, plus LRU age).
// -------------------------------
struct TLBEntry {
    bool     valid = false;   // whether this entry is usable
    uint64_t vpn   = 0;       // virtual page number tag
    uint64_t ppn   = 0;       // physical page number payload (optional; keep if you want)
    uint64_t lru   = 0;       // larger means more recently used (we use a global "tick")
};

// -------------------------------
// A fully-associative "shadow TLB" (same total capacity as the real TLB).
// Used only to classify 3C misses.
// -------------------------------
class FullyAssocShadow {
public:
    explicit FullyAssocShadow(size_t capacity_entries)
        : cap_(capacity_entries), entries_(capacity_entries) {}

    // Lookup VPN in shadow.
    bool probe(uint64_t vpn, uint64_t tick) {
        // Linear scan is fine for typical TLB sizes (e.g., 32-4096 entries).
        for (auto &e : entries_) {
            if (e.valid && e.vpn == vpn) {
                e.lru = tick; // update LRU on hit
                return true;
            }
        }
        return false;
    }

    // Insert/update VPN in shadow (LRU replacement).
    void insert(uint64_t vpn, uint64_t ppn, uint64_t tick) {
        // If already present, update and return.
        for (auto &e : entries_) {
            if (e.valid && e.vpn == vpn) {
                e.ppn = ppn;
                e.lru = tick;
                return;
            }
        }

        // Find invalid slot first.
        for (auto &e : entries_) {
            if (!e.valid) {
                e.valid = true;
                e.vpn   = vpn;
                e.ppn   = ppn;
                e.lru   = tick;
                return;
            }
        }

        // Otherwise evict LRU (smallest lru value).
        size_t victim = 0;
        uint64_t best_lru = entries_[0].lru;
        for (size_t i = 1; i < entries_.size(); i++) {
            if (entries_[i].lru < best_lru) {
                best_lru = entries_[i].lru;
                victim = i;
            }
        }
        entries_[victim].valid = true;
        entries_[victim].vpn   = vpn;
        entries_[victim].ppn   = ppn;
        entries_[victim].lru   = tick;
    }

    void flush() {
        for (auto &e : entries_) e.valid = false;
    }

private:
    size_t cap_;
    std::vector<TLBEntry> entries_;
};

// -------------------------------
// Set-associative TLB + 3C stats.
// -------------------------------
class SetAssocTLB {
public:
    struct Stats {
        uint64_t accesses    = 0; // total lookups
        uint64_t hits        = 0; // hit count
        uint64_t misses      = 0; // miss count

        // 3C breakdown (sums to misses, unless you classify some as "other")
        uint64_t compulsory  = 0;
        uint64_t conflict    = 0;
        uint64_t capacity    = 0;
    };

    // sets: number of sets (power of two recommended)
    // ways: associativity
    // page_bytes: used to compute VPN = vaddr / page_bytes (page_bytes must be power of two)
    SetAssocTLB(size_t sets, size_t ways, size_t page_bytes)
        : sets_(sets),
          ways_(ways),
          page_bytes_(page_bytes),
          set_index_mask_(sets - 1),
          page_shift_(ilog2_pow2(page_bytes)),
          lines_(sets * ways),
          shadow_(sets * ways)  // fully-assoc shadow has SAME total entries => for 3C classification
    {
        assert(sets_ > 0 && ways_ > 0);
        assert((sets_ & (sets_ - 1)) == 0 && "sets must be power of two for cheap indexing");
        assert((page_bytes_ & (page_bytes_ - 1)) == 0 && "page size must be power of two");
    }

    // Access by virtual address. Returns hit/miss and optionally PPN (if you set it).
    // In many simulators, you might just want "hit?" and not store ppn.
    bool access(uint64_t vaddr, uint64_t ppn_payload = 0) {
        stats_.accesses++;
        tick_++; // monotonic counter used as LRU timestamp

        const uint64_t vpn = vaddr >> page_shift_;               // compute virtual page number
        const size_t set   = static_cast<size_t>(vpn) & set_index_mask_; // pick set bits
        TLBEntry* way_hit  = find_in_set(set, vpn);

        // -------------------------------
        // HIT path
        // -------------------------------
        if (way_hit) {
            stats_.hits++;
            way_hit->lru = tick_;       // update LRU
            // Keep ppn if you want to model translation caching
            // (some sims will update on hit too; optional).
            (void)ppn_payload;
            // Also touch shadow so it matches "ideal capacity" policy (optional but consistent).
            shadow_.probe(vpn, tick_);
            return true;
        }

        // -------------------------------
        // MISS path: 3C classification
        // -------------------------------
        stats_.misses++;

        // Compulsory: first time we ever reference this VPN.
        // Use an unbounded set (like ideal infinite history).
        const bool first_time = (ever_seen_.insert(vpn).second);

        if (first_time) {
            stats_.compulsory++;
        } else {
            // Not first-time: either conflict or capacity.
            // Use fully-associative shadow of same total entries:
            //   - If shadow HIT => the working set fits in capacity, but mapping to sets caused miss => conflict
            //   - If shadow MISS => even ideal placement can't hold it => capacity
            const bool shadow_hit = shadow_.probe(vpn, tick_);
            if (shadow_hit) stats_.conflict++;
            else            stats_.capacity++;
        }

        // Insert into real TLB and shadow (LRU replacement)
        insert_into_set(set, vpn, ppn_payload);
        shadow_.insert(vpn, ppn_payload, tick_);
        return false;
    }

    void flush() {
        // Invalidate all TLB lines
        for (auto &e : lines_) e.valid = false;
        // Flush shadow too, so 3C classification matches TLB flush behavior.
        shadow_.flush();
        // NOTE: ever_seen_ is NOT flushed: compulsory is "first reference ever",
        // not "first reference since flush". If you want "compulsory since flush",
        // clear ever_seen_ here.
    }

    // Access to stats
    const Stats& stats() const { return stats_; }

    void print_stats(std::ostream& os = std::cout) const {
        const double acc = static_cast<double>(stats_.accesses);
        const double miss = static_cast<double>(stats_.misses);
        os << "TLB Stats\n";
        os << "  accesses   : " << stats_.accesses << "\n";
        os << "  hits       : " << stats_.hits << "\n";
        os << "  misses     : " << stats_.misses << "\n";
        os << std::fixed << std::setprecision(4);
        os << "  hit_rate   : " << (acc ? (double)stats_.hits / acc : 0.0) << "\n";
        os << "  miss_rate  : " << (acc ? miss / acc : 0.0) << "\n";
        os << "  3C compulsory : " << stats_.compulsory
           << (miss ? " (" + pct(stats_.compulsory, stats_.misses) + "% of misses)" : "") << "\n";
        os << "  3C conflict   : " << stats_.conflict
           << (miss ? " (" + pct(stats_.conflict, stats_.misses) + "% of misses)" : "") << "\n";
        os << "  3C capacity   : " << stats_.capacity
           << (miss ? " (" + pct(stats_.capacity, stats_.misses) + "% of misses)" : "") << "\n";
    }

private:
    // Helper to format percentage
    static std::string pct(uint64_t part, uint64_t total) {
        double p = total ? (100.0 * (double)part / (double)total) : 0.0;
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(2) << p;
        return ss.str();
    }

    // Get pointer to entry if hit in the set; else nullptr.
    TLBEntry* find_in_set(size_t set, uint64_t vpn) {
        const size_t base = set * ways_;
        for (size_t w = 0; w < ways_; w++) {
            TLBEntry &e = lines_[base + w];
            if (e.valid && e.vpn == vpn) return &e;
        }
        return nullptr;
    }

    // Insert VPN into set (LRU): choose invalid first, else evict smallest lru.
    void insert_into_set(size_t set, uint64_t vpn, uint64_t ppn) {
        const size_t base = set * ways_;

        // Prefer invalid line
        for (size_t w = 0; w < ways_; w++) {
            TLBEntry &e = lines_[base + w];
            if (!e.valid) {
                e.valid = true;
                e.vpn   = vpn;
                e.ppn   = ppn;
                e.lru   = tick_;
                return;
            }
        }

        // Evict LRU line (min lru)
        size_t victim = base;
        uint64_t best_lru = lines_[base].lru;
        for (size_t w = 1; w < ways_; w++) {
            TLBEntry &e = lines_[base + w];
            if (e.lru < best_lru) {
                best_lru = e.lru;
                victim = base + w;
            }
        }

        lines_[victim].valid = true;
        lines_[victim].vpn   = vpn;
        lines_[victim].ppn   = ppn;
        lines_[victim].lru   = tick_;
    }

private:
    size_t sets_;
    size_t ways_;
    size_t page_bytes_;

    size_t set_index_mask_;   // used for set = vpn & mask (only works if sets is power of 2)
    unsigned page_shift_;     // log2(page_bytes)

    std::vector<TLBEntry> lines_; // actual set-assoc storage, size = sets * ways

    FullyAssocShadow shadow_;     // fully-assoc shadow of same capacity (3C classification)

    std::unordered_set<uint64_t> ever_seen_; // tracks whether VPN has been referenced before (compulsory)

    uint64_t tick_ = 0;           // LRU clock
    Stats stats_;
};


// // -------------------------------
// // Example usage (remove in your simulator integration)
// // -------------------------------
// int main() {
//     // Example: 64 sets, 4 ways => 256-entry TLB, 4KB pages
//     SetAssocTLB tlb(/*sets=*/64, /*ways=*/4, /*page_bytes=*/4096);

//     // Fake access stream
//     for (int i = 0; i < 10000; i++) {
//         uint64_t vaddr = (uint64_t)(i % 1024) * 4096ULL; // touch 1024 distinct pages cyclically
//         tlb.access(vaddr);
//     }

//     tlb.print_stats();
//     return 0;
// }
