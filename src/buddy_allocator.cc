#include "buddy_allocator.h"
#include "logging.h"
#include "ooo_cpu.h"
#include "uncore.h"
#include <cmath>

// Global instance
BuddyAllocator buddy_allocator;

// Populate free_lists from [BUDDY_RESERVED_PAGES, num_pages) using alignment-safe
// ascending-order fills, so every inserted block is naturally aligned to its order.
//
// For each start position we find the highest order such that:
//   (a) start is a multiple of 2^order  (alignment)
//   (b) start + 2^order <= num_pages    (fits)
//
// Example (BUDDY_RESERVED_PAGES=2^18, num_pages=2^20):
//   start=2^18  → order=18  → free_lists[18]={2^18},  start=2^18+2^18=2^19
//   start=2^19  → order=19  → free_lists[19]={2^19},  start=2^19+2^19=2^20 → done
//   (Total: 2^18+2^19 = 3/4 of DRAM, all properly aligned)
void BuddyAllocator::init(uint64_t num_pages)
{
    // static_assert((uint64_t)DRAM_PAGES <= (1ULL << BUDDY_MAX_ORDER),
    //     "BUDDY_MAX_ORDER too small for DRAM_PAGES — increase BUDDY_MAX_ORDER");

    if((uint64_t)DRAM_PAGES > (1ULL << BUDDY_MAX_ORDER))
    {
        BUDDY_MAX_ORDER = log(DRAM_PAGES);
    }
    total_pages = num_pages;
    // Skip [0, BUDDY_RESERVED_PAGES) so reserved frames never enter the free lists.
    uint64_t start = BUDDY_RESERVED_PAGES;
    while (start < num_pages) {
        // Find the highest order at which 'start' is aligned and the block fits.
        int order = 0;
        while (order < BUDDY_MAX_ORDER) {
            uint64_t next_size = 1ULL << (order + 1);
            if (start & (next_size - 1))     break; // start not aligned to 2^(order+1)
            if (start + next_size > num_pages) break; // block would exceed capacity
            order++;
        }
        free_lists[order].insert(start);
        start += 1ULL << order;
    }
}

// Allocate any free order-0 page from the buddy free lists.
// Splits higher-order blocks as needed.
// Returns the physical frame number, or UINT64_MAX if no free page is available.
uint64_t BuddyAllocator::alloc_any()
{
    for (int order = 0; order <= BUDDY_MAX_ORDER; order++) {
        if (free_lists[order].empty()) continue;
        uint64_t block = *free_lists[order].begin();
        free_lists[order].erase(free_lists[order].begin());
        // Split down to order-0, releasing the upper half at each level.
        while (order > 0) {
            order--;
            uint64_t upper = block + (1ULL << order);
            free_lists[order].insert(upper);
        }
        return block;
    }
    return UINT64_MAX; // out of physical memory
}

// Free page_num and merge with buddy blocks of 2^i until no merge is possible
void BuddyAllocator::free_page(uint64_t page_num)
{
    int order = 0;
    uint64_t block = page_num;
    while (order < BUDDY_MAX_ORDER) {
        uint64_t buddy = block ^ (1ULL << order);
        if (buddy < total_pages && free_lists[order].count(buddy)) {
            free_lists[order].erase(buddy);
            block = std::min(block, buddy); // merged block starts at lower address
            shadow_clear_evicted_page(page_num);
            order++;
        } else {
            break;
        }
    }
    free_lists[order].insert(block);
}

void BuddyAllocator::shadow_clear_evicted_page(uint64_t pa)
{
    uint64_t page_key;
    uint32_t block_idx, entry_idx;
    pte_addr_decompose(pa, page_key, block_idx, entry_idx);

    for(int i=0; i< NUM_SHADOW_PT_LEVELS; i++)
    {
        auto it = shadow_pt[i].find(page_key);
        if(it != shadow_pt[i].end())
        {
            ShadowPTBlock& shd_blocks = it->second.blocks[block_idx];
            for(int j=0; j< SHADOW_ENTRIES_PER_BLOCK; j++){
                shd_blocks.entries[j].value = shd_blocks.entries[j].access = shd_blocks.entries[j].consume_pf_cost = 0;
                shd_blocks.entries[j].page_fault = PTEStatus::FAULT;
            }
        }
    }
}

// Remove every cached copy of a page before its physical frame is reused.
// Translation caches are virtually tagged, while the data caches are tagged
// with physical cache-line addresses, so both sides of the mapping are needed.
void BuddyAllocator::invalidate_evicted_page(uint64_t pframe_num, uint64_t vpage_num)
{
    for (uint32_t cpu = 0; cpu < NUM_CPUS; cpu++) {
        if (vpage_num != UINT64_MAX) {
            ooo_cpu[cpu].ITLB.invalidate_entry(vpage_num);
            ooo_cpu[cpu].DTLB.invalidate_entry(vpage_num);
            ooo_cpu[cpu].STLB.invalidate_entry(vpage_num);
            ooo_cpu[cpu].shadowSTLB.invalidate_entry(vpage_num >> 2);
        }

        const uint64_t first_cache_line = pframe_num << (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE);
        const uint64_t cache_lines_per_page = PAGE_SIZE / BLOCK_SIZE;
        for (uint64_t offset = 0; offset < cache_lines_per_page; offset++) {
            const uint64_t cache_line = first_cache_line + offset;
            ooo_cpu[cpu].L1I.invalidate_entry(cache_line);
            ooo_cpu[cpu].L1D.invalidate_entry(cache_line);
            ooo_cpu[cpu].L2C.invalidate_entry(cache_line);
        }
    }

    const uint64_t first_cache_line = pframe_num << (LOG2_PAGE_SIZE - LOG2_BLOCK_SIZE);
    const uint64_t cache_lines_per_page = PAGE_SIZE / BLOCK_SIZE;
    for (uint64_t offset = 0; offset < cache_lines_per_page; offset++)
        uncore.LLC.invalidate_entry(first_cache_line + offset);
}

// Allocate any free physical page from the buddy free lists.
// Evicts the oldest allocated frame (FIFO) when at capacity.
// Returns the physical byte address of the newly allocated frame, 0 on OOM.
uint64_t BuddyAllocator::access()
{
    if (total_pages == 0)
        init(DRAM_PAGES);

    uint64_t allocatable = total_pages - BUDDY_RESERVED_PAGES;

    // Evict the oldest physical frame when at capacity.
    if (allocated.size() >= allocatable) {
        uint64_t evict = alloc_fifo.front();
        alloc_fifo.pop_front();
        allocated.erase(evict);
        // Remove any vpage→pframe mapping for the evicted frame.
        auto rit = pframe_to_vpage.find(evict);
        uint64_t evicted_vpage = UINT64_MAX;
        if (rit != pframe_to_vpage.end()) {
            evicted_vpage = rit->second;
            vpage_to_pframe.erase(rit->second);
            pframe_to_vpage.erase(rit);
        }
        invalidate_evicted_page(evict, evicted_vpage);

        free_page(evict);
    }

    uint64_t pframe = alloc_any();
    if (pframe == UINT64_MAX)
        return 0; // out of memory — should not happen if capacity was checked

    allocated.insert(pframe);
    alloc_fifo.push_back(pframe);
    return pframe << LOG2_PAGE_SIZE;
}

// ---------------------------------------------------------------------------
// Shadow page-table implementation
// ---------------------------------------------------------------------------

// Register page_key as belonging to a page-table page at radix 'level'.
// Every entry starts with page_fault=true and value=0.
// The real physical address is stored later by shadow_set_entry() when the
// corresponding DRAM request completes and buddy_allocator.access() runs.
void BuddyAllocator::shadow_init_page(uint64_t page_key, uint8_t level)
{
    ShadowPTPage &pg = shadow_pt[level][page_key];
    if (pg.initialized)
        return; // already set up — preserve existing entries

    l.log("ShadowInitPage", " level=", +level, " page_key=", hex2str(page_key), '\n');

    pg.level       = level;
    pg.initialized = true;
    // All ShadowPTEntry members default-construct to {value=0, page_fault=false}.
    // Mark every entry as page_fault=true: the page has not been allocated yet.
    for (uint32_t b = 0; b < SHADOW_BLOCKS_PER_PAGE; b++)
        for (uint32_t e = 0; e < SHADOW_ENTRIES_PER_BLOCK; e++)
            pg.blocks[b].entries[e].page_fault = PTEStatus::FAULT;
}

// Overwrite the shadow entry for the PTE at exact byte address pte_paddr.
// Hack:::
// Defaul PTEStatus pte_status = NO_FAULT, we are doing this to use intermediate state we 
// needed for ddrp packet, since it was earlier setting NO_FAULT and regular request was
// dicovering Hit even though that PTE was invalid earlier
void BuddyAllocator::shadow_set_entry(uint64_t pte_paddr, uint8_t level, uint64_t value, PTEStatus pte_status)
{
    uint64_t page_key;
    uint32_t block_idx, entry_idx;
    pte_addr_decompose(pte_paddr, page_key, block_idx, entry_idx);

    auto it = shadow_pt[level].find(page_key);
    if (it != shadow_pt[level].end()) {
        it->second.blocks[block_idx].entries[entry_idx].value      = value;
        it->second.blocks[block_idx].entries[entry_idx].page_fault = pte_status; // explicit set = officially known

        if(it->second.blocks[block_idx].entries[entry_idx].access)
        {
            it->second.blocks[block_idx].entries[entry_idx].consume_pf_cost = 1;
        }
        it->second.blocks[block_idx].entries[entry_idx].access = false; // first access done
        
        // std::string status_pf_entries = "";
        // for(auto entry: it->second.blocks[block_idx].entries){
        //     status_pf_entries += std::to_string(entry.page_fault)+"_";
        // }
    }
    // If the page hasn't been initialised yet we silently drop the write;
    // shadow_init_page must be called first (done in ptw.cc operate()).
}

// Read the shadow entry for the PTE at exact byte address pte_paddr.
// Returns true (found pte) and sets 'value' and 'is_page_fault' if the page is tracked.
bool BuddyAllocator::shadow_get_entry(uint64_t pte_paddr, uint8_t level, uint64_t &value, bool &is_page_fault, bool &first_access) const
{
    uint64_t page_key;
    uint32_t block_idx, entry_idx;
    pte_addr_decompose(pte_paddr, page_key, block_idx, entry_idx);

    auto it = shadow_pt[level].find(page_key);
    if (it == shadow_pt[level].end())
        return false;

    const ShadowPTEntry &e = it->second.blocks[block_idx].entries[entry_idx];
    value        = e.value;
    is_page_fault = e.page_fault == PTEStatus::FAULT || e.page_fault == PTEStatus::DDRP_PROXY;
    first_access = e.access;
    return true;
}

// Convenience overload — page_fault flag is ignored by the caller.
bool BuddyAllocator::shadow_get_entry(uint64_t pte_paddr, uint8_t level, uint64_t &value) const
{
    bool pf, fa;
    return shadow_get_entry(pte_paddr, level, value, pf, fa);
}

// Convenience overload — page_fault flag is ignored by the caller.
bool BuddyAllocator::shadow_apply_pf_cost(uint64_t pte_paddr, uint8_t level)
{
    uint64_t page_key;
    uint32_t block_idx, entry_idx;
    pte_addr_decompose(pte_paddr, page_key, block_idx, entry_idx);

    auto it = shadow_pt[level].find(page_key);
    if (it == shadow_pt[level].end())
        return false;

    ShadowPTEntry &e = it->second.blocks[block_idx].entries[entry_idx];
    bool t = e.consume_pf_cost;
    e.consume_pf_cost = false;
    return t;
}


std::string BuddyAllocator::debug_print_valid_block_entry(uint64_t pte_paddr, uint8_t level)
{
    uint64_t page_key;
    uint32_t block_idx, entry_idx;
    pte_addr_decompose(pte_paddr, page_key, block_idx, entry_idx);

    std::string status_pf_entries = "";
    auto it = shadow_pt[level].find(page_key);
    if (it != shadow_pt[level].end()){
        for(auto entry: it->second.blocks[block_idx].entries){
            status_pf_entries += std::to_string(entry.page_fault)+"_";
        }
    }
    return status_pf_entries;
}
