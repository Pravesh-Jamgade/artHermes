#ifndef BUDDY_ALLOCATOR_H
#define BUDDY_ALLOCATOR_H

#include <unordered_set>
#include <unordered_map>
#include <deque>
#include <cstdint>
#include <algorithm>
#include "defs.h"
#include "commons.h"
#include <string>
#include <map>

#define PAGE_FAULT_LATENCY   1000                   // cycles added on first access to a page
#define BUDDY_RESERVED_PAGES ((1ULL << 30) / 4096) // 1GB reserved region: pages 0..262143 are off-limits

inline int BUDDY_MAX_ORDER = 20;     // supports up to 2^20 pages (4GB at 4KB/page)
// ---------------------------------------------------------------------------
// Shadow page-table structure
//
// Every page-table page (at any radix level) is modelled as an array of
// SHADOW_BLOCKS_PER_PAGE cache blocks, each holding SHADOW_ENTRIES_PER_BLOCK
// 8-byte PTE entries.
//
//   PAGE_SIZE       = 4096 bytes
//   BLOCK_SIZE      =   64 bytes  →  SHADOW_BLOCKS_PER_PAGE    = 64
//   sizeof(uint64_t)=    8 bytes  →  SHADOW_ENTRIES_PER_BLOCK  =  8
// ---------------------------------------------------------------------------
#define SHADOW_ENTRIES_PER_BLOCK  (BLOCK_SIZE / sizeof(uint64_t))   // 8
#define SHADOW_BLOCKS_PER_PAGE    (PAGE_SIZE  / BLOCK_SIZE)         // 64
#define NUM_SHADOW_PT_LEVELS      5                                  // levels 0..4 (1=PT … 4=PML4)


enum PTEStatus{
    NO_FAULT=0,
    FAULT,
    DDRP_PROXY
};

struct ShadowPTEntry {
    uint64_t value;       // physical address / PTE value
    PTEStatus page_fault;  // true until the PTW officially walks this entry
    bool access;    // first time accessed entry ?
    bool consume_pf_cost;
    ShadowPTEntry() : value(0), access(true), page_fault(PTEStatus::FAULT), consume_pf_cost(0) {}
};

struct ShadowPTBlock {
    ShadowPTEntry entries[SHADOW_ENTRIES_PER_BLOCK]; // 8 PTEs per cache line
    ShadowPTBlock() {} // entries default-constructed above
};

struct ShadowPTPage {
    uint8_t       level;                             // PTW radix level (0=PT … 3=PML4)
    bool          initialized;
    ShadowPTBlock blocks[SHADOW_BLOCKS_PER_PAGE];    // 64 cache blocks = 512 PTEs total

    ShadowPTPage() : level(0), initialized(false) {}
};

struct BuddyAllocator {
    uint64_t total_pages;
    std::unordered_set<uint64_t> free_lists[100]; // free_lists[i] = base addrs of free 2^i-page blocks
    std::unordered_set<uint64_t> allocated;       // resident physical frames
    std::deque<uint64_t>         alloc_fifo;      // FIFO order for physical frame eviction
    std::map<std::tuple<uint64_t, int>, uint64_t> intermediate_mapping; // <pte addr + level> to pte val mapping
    std::unordered_map<uint64_t, uint64_t> vpage_to_pframe; // vpage → pframe (data-page residency)
    std::unordered_map<uint64_t, uint64_t> pframe_to_vpage; // pframe → vpage (reverse, for eviction cleanup)

    // Shadow page-table: one ShadowPTPage per page-table page frame, kept per PTW level.
    // Key = page_key = (PTE byte address) >> LOG2_PAGE_SIZE.
    // Indexed by PTW radix level (0..NUM_SHADOW_PT_LEVELS-1).
    std::unordered_map<uint64_t, ShadowPTPage> shadow_pt[NUM_SHADOW_PT_LEVELS];

    BuddyAllocator() : total_pages(0) {}
    void init(uint64_t num_pages);
    // Allocate any free physical page from the buddy free lists.
    // Handles eviction (FIFO) if at capacity.
    // Returns the physical byte address of the allocated frame (0 on OOM).
    uint64_t access();

    // Look up the physical byte address for a previously mapped virtual page.
    uint64_t get_pframe_addr(uint64_t vpage_num) const {
        auto it = vpage_to_pframe.find(vpage_num);
        return it != vpage_to_pframe.end() ? (it->second << LOG2_PAGE_SIZE) : 0;
    }

    void map_vpage_to_pframe(uint64_t vpage_num, uint64_t pframe_num) {
        vpage_to_pframe[vpage_num] = pframe_num;
        pframe_to_vpage[pframe_num] = vpage_num;
    }

    // ---------------------------------------------------------------------------
    // Shadow page-table interface
    //
    // shadow_init_page – register a page-table page at a given radix level.
    //   Idempotent: a second call for the same page_key is silently ignored.
    //
    // shadow_set_entry – overwrite one 8-byte PTE entry identified by its exact
    //   physical byte address (pte_paddr).
    //
    // shadow_get_entry – read one 8-byte PTE entry; returns false if the page
    //   has not been initialised yet.
    // ---------------------------------------------------------------------------
    void shadow_init_page(uint64_t page_key, uint8_t level);
    void shadow_set_entry(uint64_t pte_paddr, uint8_t level, uint64_t value, PTEStatus pte_status = PTEStatus::NO_FAULT);
    bool shadow_get_entry(uint64_t pte_paddr, uint8_t level, uint64_t &value, bool &is_page_fault, bool& is_first_access) const;
    bool shadow_get_entry(uint64_t pte_paddr, uint8_t level, uint64_t &value) const;
    // Todo: when page is evicted we should be reseting entrys bits: accessm, consume_pf, etc
    // right now buddyalloc only stores pa->va mapping, but we lack level information with 
    // (and in worst case we might need to keep cpu/thread information with it)
    // using pa+level we can get and reset shadow entry.
    // it is possible to iterate and search levels as well using just pa (as pa in the end is just the unique number 4KB apart)
    void shadow_clear_evicted_page(uint64_t pte_paddr);
    std::string debug_print_valid_block_entry(uint64_t pte_paddr, uint8_t level);
    bool shadow_apply_pf_cost(uint64_t pte_paddr, uint8_t level);

private:
    uint64_t alloc_any(); // allocate any available order-0 page from the buddy free lists
    void free_page(uint64_t page_num);

    // Decompose a PTE byte address into its shadow indices.
    static void pte_addr_decompose(uint64_t pte_paddr,
                                   uint64_t &page_key,
                                   uint32_t &block_idx,
                                   uint32_t &entry_idx)
    {
        page_key  =  pte_paddr >> LOG2_PAGE_SIZE;
        block_idx = (pte_paddr >> LOG2_BLOCK_SIZE) & (SHADOW_BLOCKS_PER_PAGE   - 1); // bits[11:6]
        entry_idx = (pte_paddr &  (BLOCK_SIZE - 1)) >> 3;                             // bits[ 5:3]
    }
};

extern BuddyAllocator buddy_allocator;

#endif /* BUDDY_ALLOCATOR_H */
