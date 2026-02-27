#ifndef PTW_H
#define PTW_H

#include <deque>
#include <vector>
#include <unordered_map>
#include "champsim.h"
#include "defs.h"
#include "block.h"

// Page Walk Cache (PwC) configuration for 4 levels
// Each level corresponds to page table level (4, 3, 2, 1 in x86-64)
#define PWC_L4_SETS 64      // PML4 level cache
#define PWC_L4_WAYS 4
#define PWC_L3_SETS 128     // PDPT level cache
#define PWC_L3_WAYS 4
#define PWC_L2_SETS 256     // PD level cache
#define PWC_L2_WAYS 8
#define PWC_L1_SETS 512     // PT level cache
#define PWC_L1_WAYS 8

#define PWC_TOTAL_LEVELS 4

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

    // page allocation tracking (57-bit physical addresses)
    std::unordered_map<uint64_t, uint64_t> vpage_to_ppage; // virtual page -> physical page
    uint64_t next_free_ppage; // simple sequential allocator

    // allocate a new physical page
    uint64_t allocate_page();

    
    // 4 levels of Page Walk Cache
    PageWalkCacheLevel level_caches[PWC_TOTAL_LEVELS];
    
    // Outstanding page walk requests
    struct OutstandingWalk {
        uint64_t vaddr;
        uint64_t current_level;
        uint64_t current_pa;
        uint64_t requested_cycle;
        uint32_t instr_id;
        PACKET *packet;
        bool waiting;           // waiting for memory response
        
        // Per-level statistics for this walk
        struct LevelStats {
            uint64_t access_count;  // accesses to this level
            uint64_t hit_count;     // cache hits at this level
            uint64_t miss_count;    // cache misses at this level
            uint8_t hit_where;      // where result came from (PwC, L1D, L2C, LLC, DRAM)
            uint8_t page_fault;     // 1 if this level had a page fault
        } level_stats[PWC_TOTAL_LEVELS];
        
        uint64_t total_page_faults;
        
        OutstandingWalk() : total_page_faults(0) {
            for (uint32_t i = 0; i < PWC_TOTAL_LEVELS; i++) {
                level_stats[i].access_count = 0;
                level_stats[i].hit_count = 0;
                level_stats[i].miss_count = 0;
                level_stats[i].hit_where = 0;
                level_stats[i].page_fault = 0;
            }
        }
    };
    
    std::deque<OutstandingWalk> outstanding_walks;
    
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
    
    // Process page walk for a specific level
    uint64_t walk_page_table_level(uint64_t vaddr, uint32_t level, uint64_t current_pa);
    
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
    void complete_page_walk(OutstandingWalk &walk, uint64_t translated_pa);
    
    // Get set index for a PwC level
    uint32_t get_pwc_set_index(uint64_t vaddr, uint32_t level);
    
    // Extract virtual address components for different levels
    uint64_t get_level_vaddr_tag(uint64_t vaddr, uint32_t level);
    
    // Get the next level page table physical address from current PTE
    uint64_t extract_next_level_pa(uint64_t pte);
    
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
    
    // Helper function to get PwC level parameters
    void get_level_params(uint32_t level, uint32_t &sets, uint32_t &ways);
};

#endif // PTW_H
