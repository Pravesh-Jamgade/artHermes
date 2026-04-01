#ifndef PTW_H
#define PTW_H

#include <deque>
#include <vector>
#include <unordered_map>
#include "champsim.h"
#include "defs.h"
#include "block.h"

// Page Walk Cache (PwC) configuration for 4 levels
// Fully associative (single set) per level; total entries preserved.
// Each level corresponds to page table level (4, 3, 2, 1 in x86-64)
#define PWC_L4_SETS 1       // PML4 level cache
#define PWC_L4_WAYS 8
#define PWC_L3_SETS 1       // PDPT level cache
#define PWC_L3_WAYS 8
#define PWC_L2_SETS 1       // PD level cache
#define PWC_L2_WAYS 16
#define PWC_L1_SETS 1       // PT level cache
#define PWC_L1_WAYS 16

#define PWC_TOTAL_LEVELS 4
#define PTW_MSHR_SIZE    16

// Shared per-level stats carried by both OutstandingWalk and PTW_MSHR_Entry
struct PTW_LevelStats {
    uint64_t access_count;  // accesses to this level
    uint64_t hit_count;     // cache hits at this level
    uint64_t miss_count;    // cache misses at this level
    uint8_t  hit_where;     // where result came from (0=PwC, 1=L1D, 2=L2C, 3=LLC, 4=DRAM)
    uint8_t  page_fault;    // 1 if this level had a page fault
    PTW_LevelStats() : access_count(0), hit_count(0), miss_count(0), hit_where(0), page_fault(0) {}
};

// PwC entry for level-specific translations
struct PwC_Entry {
    uint64_t vaddr;              // Virtual address of the page table entry
    uint64_t paddr;              // Physical address (translation result)
    uint64_t tag;                // Tag for set-associative lookup
    uint32_t lru;                // LRU counter for replacement
    uint8_t valid;               // Valid bit
    uint32_t level;              // Which level this entry belongs to (1-4)
    
    PwC_Entry() : vaddr(0), paddr(0), tag(0), lru(0), valid(0), level(0) {}
};

// PwC level structure
struct PageWalkCacheLevel {
    uint32_t sets;
    uint32_t ways;
    std::vector<std::vector<PwC_Entry>> entries;
    uint64_t hits;
    uint64_t misses;
    uint64_t replacements;
    
    PageWalkCacheLevel() : sets(0), ways(0), hits(0), misses(0), replacements(0) {}

    PageWalkCacheLevel(uint32_t s, uint32_t w) : sets(s), ways(w), hits(0), misses(0), replacements(0) {
        entries.resize(sets, std::vector<PwC_Entry>(ways));
    }
};

// PTW (Page Table Walker) module
class PTWclass {
public:
    uint32_t cpu;
    
    // Pointers to TLB caches for returning completed translations
    class CACHE *stlb_cache;
    class CACHE *dtlb_cache;
    class CACHE *itlb_cache;

    // CPU-indexed CR3 register base addresses (100GB stride)
    uint64_t cr3_base_addrs[NUM_CPUS];

    
    // 4 levels of Page Walk Cache
    PageWalkCacheLevel level_caches[PWC_TOTAL_LEVELS];
    
    // Outstanding page walk requests (pending, not yet dispatched to memory)
    struct OutstandingWalk {
        uint64_t vaddr;
        int      current_level;   // level to process next (3=PML4 down to 0=PT)
        uint64_t current_pa;      // page-table base address for current_level
        uint64_t requested_cycle;
        uint32_t instr_id;
        PACKET   packet;          // original STLB packet, copied by value
        PTW_LevelStats level_stats[PWC_TOTAL_LEVELS];
        uint64_t total_page_faults;
        OutstandingWalk() : vaddr(0), current_level(3), current_pa(0),
                            requested_cycle(0), instr_id(0), total_page_faults(0) {}
    };

    // MSHR entry for in-flight page table memory requests
    // Fixed-size array ensures pointer stability (ptw_walk_ptr points here)
    struct PTW_MSHR_Entry {
        bool     valid;
        bool     piggyback;      // true = did NOT send its own L1D request; waits for a
                                 // primary entry at the same (current_pa, current_level)
        uint64_t vaddr;
        int      current_level;  // level of the in-flight PTE request
        uint64_t current_pa;     // PTE byte address sent to memory (PwC insert key)
        uint64_t table_base_pa;  // page-table base for current_level (to re-queue walk)
        uint64_t requested_cycle;
        uint32_t instr_id;
        PACKET   packet;         // original STLB packet, copied by value
        PTW_LevelStats level_stats[PWC_TOTAL_LEVELS];
        uint64_t total_page_faults;
        PTW_MSHR_Entry() : valid(false), piggyback(false), vaddr(0), current_level(0),
                           current_pa(0), table_base_pa(0),
                           requested_cycle(0), instr_id(0), total_page_faults(0) {}
    };

    std::deque<OutstandingWalk> outstanding_walks;
    PTW_MSHR_Entry mshr[PTW_MSHR_SIZE];  // fixed-size, pointer-stable
    
    // Statistics
    struct {
        uint64_t total_walks_initiated;
        uint64_t walks_completed;
        uint64_t walks_stalled;
        uint64_t pwc_l4_hits;
        uint64_t pwc_l4_misses;
        uint64_t pwc_l3_hits;
        uint64_t pwc_l3_misses;
        uint64_t pwc_l2_hits;
        uint64_t pwc_l2_misses;
        uint64_t pwc_l1_hits;
        uint64_t pwc_l1_misses;
        uint64_t total_latency;
        uint64_t max_latency;
        
        // Per-level aggregate statistics
        struct LevelAggStats {
            uint64_t access_count;
            uint64_t hit_count;
            uint64_t miss_count;
            uint64_t page_faults;
            uint64_t pwc_hits;
            uint64_t l1d_hits;
            uint64_t l1i_hits;
            uint64_t l2c_hits;
            uint64_t llc_hits;
            uint64_t dram_hits;
        } level_stats[PWC_TOTAL_LEVELS];
        
        uint64_t total_page_faults;
    } stats;
    
    // PTW configuration
    uint32_t PTW_LATENCY;
    uint32_t MAX_OUTSTANDING_WALKS;
    
    // Constructor and destructor
    PTWclass(uint32_t cpu_id);
    ~PTWclass();
    
    // Initialize PTW
    void initialize();
    
    // Initiate a page walk for a virtual address
    void initiate_page_walk(PACKET *packet, uint64_t vaddr);
    
    // Lookup in PwC at a specific level
    bool pwc_lookup(uint64_t vaddr, uint32_t level, uint64_t &paddr);
    
    // Insert into PwC at a specific level
    void pwc_insert(uint64_t vaddr, uint64_t paddr, uint32_t level);
    
    // Invalidate PwC entry
    void pwc_invalidate(uint64_t vaddr, uint32_t level);
    
    // Clear all PwC entries
    void pwc_flush();
    
    // Operate PTW (check for completed walks)
    void operate();
    
    // callback invoked when a memory request from PTW returns
    void handle_memory_response(PACKET *packet);
    
    // Complete a page walk and return to STLB
    void complete_page_walk(PACKET &pkt, uint64_t translated_pa);

    // Find a free MSHR slot; returns index or -1 if full
    int find_free_mshr();
    
    // Get set index for a PwC level
    uint64_t get_level_index(uint64_t curr_pa, uint32_t level);
    uint64_t get_level_set(uint64_t curr_pa, uint32_t level);
    
    // Extract virtual address components for different levels
    uint64_t get_level_tag(uint64_t curr_pa, uint32_t level);
  
    // Check if walk can proceed
    bool is_walk_queue_available();

    // Print PTW statistics
    void print_stats();
    void print_config();
    
private:
    // Helper function to update LRU
    void update_lru(uint32_t set, uint32_t way, uint32_t level);
    
    // Helper function to find victim
    uint32_t find_victim(uint32_t set, uint32_t level);
    
};

#endif // PTW_H
