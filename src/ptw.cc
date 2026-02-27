#include "ptw.h"
#include "champsim.h"
#include "ooo_cpu.h"
#include "cache.h"
#include "stlb_ptw_integration.h"
#include "uncore.h"
#include <iostream>
#include <algorithm>
#include <cstring>
#include <cassert>

// simple buddy-like sequential allocator maximum
static const uint64_t MAX_PPAGE = ((1ULL << 57) - 1);

using namespace std;

// Constructor
PTWclass::PTWclass(uint32_t cpu_id) 
    : cpu(cpu_id),
      PTW_LATENCY(100),
      MAX_OUTSTANDING_WALKS(16),
      stlb_cache(NULL),
      dtlb_cache(NULL),
      itlb_cache(NULL),
      next_free_ppage(0)
{
    // explicitly initialize array elements since they cannot be in the init list
    level_caches[0] = PageWalkCacheLevel(PWC_L4_SETS, PWC_L4_WAYS);
    level_caches[1] = PageWalkCacheLevel(PWC_L3_SETS, PWC_L3_WAYS);
    level_caches[2] = PageWalkCacheLevel(PWC_L2_SETS, PWC_L2_WAYS);
    level_caches[3] = PageWalkCacheLevel(PWC_L1_SETS, PWC_L1_WAYS);

    memset(&stats, 0, sizeof(stats));
    for (uint32_t i = 0; i < PWC_TOTAL_LEVELS; i++) {
        stats.level_stats[i].access_count = 0;
        stats.level_stats[i].hit_count = 0;
        stats.level_stats[i].miss_count = 0;
        stats.level_stats[i].page_faults = 0;
        stats.level_stats[i].pwc_hits = 0;
        stats.level_stats[i].l1d_hits = 0;
        stats.level_stats[i].l2c_hits = 0;
        stats.level_stats[i].llc_hits = 0;
        stats.level_stats[i].dram_hits = 0;
    }
    initialize();
}

// Destructor
PTWclass::~PTWclass() {
    print_stats();
}

// allocate next physical page in a buddy-like fashion
uint64_t PTWclass::allocate_page() {
    // wrap-around check
    if (next_free_ppage >= MAX_PPAGE) {
        // simple wrap or reuse; in real system we'd handle free list
        next_free_ppage = 0;
    }
    uint64_t allocated = next_free_ppage;
    next_free_ppage++;
    stats.total_page_faults++;  // track that we had a page fault
    return allocated;
}

// Initialize PTW
void PTWclass::initialize() {
    outstanding_walks.clear();
    
    // Initialize all PwC levels
    for (uint32_t level = 0; level < PWC_TOTAL_LEVELS; level++) {
        for (uint32_t set = 0; set < level_caches[level].sets; set++) {
            for (uint32_t way = 0; way < level_caches[level].ways; way++) {
                level_caches[level].entries[set][way].valid = 0;
                level_caches[level].entries[set][way].lru = 0;
            }
        }
    }
}

// Get set index for a PwC level
uint32_t PTWclass::get_pwc_set_index(uint64_t vaddr, uint32_t level) {
    uint32_t sets = 0;
    get_level_params(level, sets, sets);
    
    // Use bits from virtual address for indexing
    // Extract appropriate bits based on level
    uint64_t index_bits = (vaddr >> (LOG2_PAGE_SIZE + (level * 9))) & ((1ULL << 9) - 1);
    return index_bits % level_caches[level].sets;
}

// Extract virtual address tag for a level
uint64_t PTWclass::get_level_vaddr_tag(uint64_t vaddr, uint32_t level) {
    // Extract the virtual address bits corresponding to this page table level
    return (vaddr >> (LOG2_PAGE_SIZE + 9 + (level * 9))) & ((1ULL << 16) - 1);
}

// Get level parameters
void PTWclass::get_level_params(uint32_t level, uint32_t &sets, uint32_t &ways) {
    switch(level) {
        case 0: // L4
            sets = PWC_L4_SETS;
            ways = PWC_L4_WAYS;
            break;
        case 1: // L3
            sets = PWC_L3_SETS;
            ways = PWC_L3_WAYS;
            break;
        case 2: // L2
            sets = PWC_L2_SETS;
            ways = PWC_L2_WAYS;
            break;
        case 3: // L1
            sets = PWC_L1_SETS;
            ways = PWC_L1_WAYS;
            break;
        default:
            sets = PWC_L1_SETS;
            ways = PWC_L1_WAYS;
    }
}

// Lookup in PwC at a specific level
bool PTWclass::pwc_lookup(uint64_t vaddr, uint32_t level, uint64_t &paddr) {
    if (level >= PWC_TOTAL_LEVELS) return false;
    
    uint32_t set = get_pwc_set_index(vaddr, level);
    uint64_t tag = get_level_vaddr_tag(vaddr, level);
    
    for (uint32_t way = 0; way < level_caches[level].ways; way++) {
        PwC_Entry &entry = level_caches[level].entries[set][way];
        
        if (entry.valid && entry.tag == tag) {
            paddr = entry.paddr;
            level_caches[level].hits++;
            update_lru(set, way, level);
            return true;
        }
    }
    
    level_caches[level].misses++;
    return false;
}

// Insert into PwC at a specific level
void PTWclass::pwc_insert(uint64_t vaddr, uint64_t paddr, uint32_t level) {
    if (level >= PWC_TOTAL_LEVELS) return;
    
    uint32_t set = get_pwc_set_index(vaddr, level);
    uint64_t tag = get_level_vaddr_tag(vaddr, level);
    
    // Check if entry already exists
    for (uint32_t way = 0; way < level_caches[level].ways; way++) {
        PwC_Entry &entry = level_caches[level].entries[set][way];
        
        if (entry.valid && entry.tag == tag) {
            entry.paddr = paddr;
            update_lru(set, way, level);
            return;
        }
    }
    
    // Find victim way
    uint32_t victim_way = find_victim(set, level);
    
    PwC_Entry &victim = level_caches[level].entries[set][victim_way];
    victim.valid = 1;
    victim.vaddr = vaddr;
    victim.paddr = paddr;
    victim.tag = tag;
    victim.level = level;
    victim.lru = 0;
    
    level_caches[level].replacements++;
    update_lru(set, victim_way, level);
}

// Invalidate PwC entry
void PTWclass::pwc_invalidate(uint64_t vaddr, uint32_t level) {
    if (level >= PWC_TOTAL_LEVELS) return;
    
    uint32_t set = get_pwc_set_index(vaddr, level);
    uint64_t tag = get_level_vaddr_tag(vaddr, level);
    
    for (uint32_t way = 0; way < level_caches[level].ways; way++) {
        PwC_Entry &entry = level_caches[level].entries[set][way];
        
        if (entry.valid && entry.tag == tag) {
            entry.valid = 0;
            return;
        }
    }
}

// Flush all PwC entries
void PTWclass::pwc_flush() {
    for (uint32_t level = 0; level < PWC_TOTAL_LEVELS; level++) {
        for (uint32_t set = 0; set < level_caches[level].sets; set++) {
            for (uint32_t way = 0; way < level_caches[level].ways; way++) {
                level_caches[level].entries[set][way].valid = 0;
            }
        }
    }
}

// Update LRU for a specific entry
void PTWclass::update_lru(uint32_t set, uint32_t way, uint32_t level) {
    // Increment all LRU counters in the set
    for (uint32_t w = 0; w < level_caches[level].ways; w++) {
        if (w != way && level_caches[level].entries[set][w].valid) {
            level_caches[level].entries[set][w].lru++;
        }
    }
    // Reset the accessed way's LRU counter
    level_caches[level].entries[set][way].lru = 0;
}

// Find victim way using LRU
uint32_t PTWclass::find_victim(uint32_t set, uint32_t level) {
    uint32_t victim_way = 0;
    uint32_t max_lru = 0;
    uint32_t ways = level_caches[level].ways;
    
    // First, look for an invalid entry
    for (uint32_t way = 0; way < ways; way++) {
        if (!level_caches[level].entries[set][way].valid) {
            return way;
        }
    }
    
    // If all valid, find the one with highest LRU count
    for (uint32_t way = 0; way < ways; way++) {
        if (level_caches[level].entries[set][way].lru > max_lru) {
            max_lru = level_caches[level].entries[set][way].lru;
            victim_way = way;
        }
    }
    
    return victim_way;
}

// Initiate a page walk for a virtual address
void PTWclass::initiate_page_walk(PACKET *packet, uint64_t vaddr) {
    if (!is_walk_queue_available()) {
        return; // Queue is full
    }
    
    OutstandingWalk new_walk;
    new_walk.vaddr = vaddr;
    new_walk.current_level = 0; // Start from L4 (PML4)
    new_walk.current_pa = 0;    // Will be filled from CR3
    new_walk.requested_cycle = current_core_cycle[cpu];
    new_walk.instr_id = packet->instr_id;
    new_walk.packet = packet;
    new_walk.waiting = false;
    
    outstanding_walks.push_back(new_walk);
    stats.total_walks_initiated++;
}

// Walk page table at a specific level
uint64_t PTWclass::walk_page_table_level(uint64_t vaddr, uint32_t level, uint64_t current_pa) {
    uint64_t translated_pa = 0;
    
    // First consult PwC cache
    if (pwc_lookup(vaddr, level, translated_pa)) {
        return translated_pa;
    }

    // compute page table entry physical address
    uint64_t level_offset = (vaddr >> (LOG2_PAGE_SIZE + (level * 9))) & ((1ULL << 9) - 1);
    uint64_t pte_pa = (current_pa + (level_offset << 3)) & ((1ULL << 52) - 1);

    // try to locate the PTE in the normal cache hierarchy (L1/L2/LLC)
    PACKET pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.address = pte_pa >> LOG2_BLOCK_SIZE;
    pkt.full_addr = pte_pa;
    pkt.cpu = cpu;
    pkt.is_data = 1;
    pkt.type = LOAD;
    pkt.instruction = 0;
    pkt.tlb_access = 0;
    pkt.fill_level = FILL_L1;

    // search L1D cache for the entry
    {
        int way = ooo_cpu[cpu].L1D.check_hit(&pkt);
        if (way != -1) {
            uint32_t set = ooo_cpu[cpu].L1D.get_set(pkt.address);
            translated_pa = ooo_cpu[cpu].L1D.block[set][way].data;
            pwc_insert(vaddr, translated_pa, level);
            return translated_pa;
        }
        // if not in L1D check L2 cache
        way = ooo_cpu[cpu].L2C.check_hit(&pkt);
        if (way != -1) {
            uint32_t set = ooo_cpu[cpu].L2C.get_set(pkt.address);
            translated_pa = ooo_cpu[cpu].L2C.block[set][way].data;
            pwc_insert(vaddr, translated_pa, level);
            return translated_pa;
        }
        // finally check LLC (shared cache in uncore)
        way = uncore.LLC.check_hit(&pkt);
        if (way != -1) {
            int32_t set = uncore.LLC.get_set(pkt.address);
            translated_pa = uncore.LLC.block[set][way].data;
            pwc_insert(vaddr, translated_pa, level);
            return translated_pa;
        }
    }
    // we could extend to search L2C, LLC etc. here by instantiating dummy lookups.

    // if not found in caches, we will simulate walking to next level
    if (level < PWC_TOTAL_LEVELS - 1) {
        // non-leaf levels: just compute pseudo address for the next page table
        translated_pa = pte_pa; // using current formula
    } else {
        // leaf level: need a real physical page mapping
        uint64_t vpage = vaddr >> LOG2_PAGE_SIZE;
        auto it = vpage_to_ppage.find(vpage);
        if (it == vpage_to_ppage.end()) {
            uint64_t new_ppage = allocate_page();
            vpage_to_ppage[vpage] = new_ppage;
            it = vpage_to_ppage.find(vpage);
        }
        translated_pa = (it->second << LOG2_PAGE_SIZE) | (vaddr & ((1ULL << LOG2_PAGE_SIZE) - 1));
    }

    // record in PwC for future
    pwc_insert(vaddr, translated_pa, level);
    
    return translated_pa;
}

// Extract next level PA from PTE
uint64_t PTWclass::extract_next_level_pa(uint64_t pte) {
    // Extract physical address from PTE (bits 12-51 in x86-64)
    return (pte & 0xFFFFFFFFF000ULL);
}

// Check if walk queue is available
bool PTWclass::is_walk_queue_available() {
    return outstanding_walks.size() < MAX_OUTSTANDING_WALKS;
}

// Operate PTW (process outstanding walks)
void PTWclass::operate() {
    vector<size_t> completed_indices;

    for (size_t i = 0; i < outstanding_walks.size(); i++) {
        OutstandingWalk &walk = outstanding_walks[i];

        // if currently waiting for a memory response skip
        if (walk.waiting)
            continue;

        // if walk already done skip
        if (walk.current_level >= PWC_TOTAL_LEVELS) {
            continue;
        }

        // attempt to resolve at current level
        uint64_t pa;
        uint32_t curr_lvl = walk.current_level;
        walk.level_stats[curr_lvl].access_count++;
        stats.level_stats[curr_lvl].access_count++;
        
        if (pwc_lookup(walk.vaddr, curr_lvl, pa)) {
            // PwC hit
            walk.level_stats[curr_lvl].hit_count++;
            walk.level_stats[curr_lvl].hit_where = 0;  // 0 = PwC
            stats.level_stats[curr_lvl].hit_count++;
            stats.level_stats[curr_lvl].pwc_hits++;
            
            // cached, advance to next level
            walk.current_pa = pa;
            walk.current_level++;
            if (walk.current_level == PWC_TOTAL_LEVELS) {
                complete_page_walk(walk, walk.current_pa);
                completed_indices.push_back(i);
            }
            continue;
        }
        
        // PwC miss
        walk.level_stats[curr_lvl].miss_count++;
        stats.level_stats[curr_lvl].miss_count++;

        // PwC miss: generate a memory request for this level
        uint64_t level_offset = (walk.vaddr >> (LOG2_PAGE_SIZE + (walk.current_level * 9))) & ((1ULL << 9) - 1);
        uint64_t pte_pa = (walk.current_pa + (level_offset << 3)) & ((1ULL << 52) - 1);

        PACKET req;
        memset(&req, 0, sizeof(req));
        req.address = pte_pa >> LOG2_BLOCK_SIZE;
        req.full_addr = pte_pa;
        req.cpu = cpu;
        req.is_data = 1;              // data cache access
        req.type = TRANSLATION;
        req.instruction = 0;
        req.tlb_access = 1;           // mark as translation packet
        req.fill_level = FILL_L1;
        req.fill_l1d = 1;
        req.from_ptw = 1;
        req.ptw_level = walk.current_level;
        req.ptw_walk_ptr = (void *)&walk;

        // issue to L1 data cache
        ooo_cpu[cpu].L1D.add_rq(&req);

        // mark walk as waiting
        walk.waiting = true;
    }

    // remove completed walks in reverse order
    for (int j = completed_indices.size() - 1; j >= 0; j--) {
        outstanding_walks.erase(outstanding_walks.begin() + completed_indices[j]);
    }
}

// handle memory response for a previously issued PTE request
void PTWclass::handle_memory_response(PACKET *packet) {
    if (packet == NULL)
        return;
    if (!packet->from_ptw)
        return;

    OutstandingWalk *walk = (OutstandingWalk *)packet->ptw_walk_ptr;
    if (walk == NULL) {
        return;
    }

    uint32_t lvl = packet->ptw_level;
    uint64_t value = packet->data; // the returned PTE/PPN

    // Track where the data came from
    uint8_t hit_src = 0;
    if (packet->hit_where == hit_where_t::L1D) {
        hit_src = 1;  // L1D
        stats.level_stats[lvl].l1d_hits++;
    } else if (packet->hit_where == hit_where_t::L2C) {
        hit_src = 2;  // L2C
        stats.level_stats[lvl].l2c_hits++;
    } else if (packet->hit_where == hit_where_t::LLC) {
        hit_src = 3;  // LLC
        stats.level_stats[lvl].llc_hits++;
    } else if (packet->hit_where == hit_where_t::DRAM) {
        hit_src = 4;  // DRAM
        stats.level_stats[lvl].dram_hits++;
    }
    
    walk->level_stats[lvl].hit_where = hit_src;

    // insert result into PwC
    pwc_insert(walk->vaddr, value, lvl);

    // update walk state
    walk->current_pa = value;
    walk->current_level = lvl + 1;
    walk->waiting = false;

    // if we just finished the last level, complete the walk
    if (walk->current_level >= PWC_TOTAL_LEVELS) {
        complete_page_walk(*walk, walk->current_pa);
        // note: actual removal from outstanding_walks happens in operate()
    }
}

// Complete a page walk
void PTWclass::complete_page_walk(OutstandingWalk &walk, uint64_t translated_pa) {
    if (walk.packet != NULL) {
        walk.packet->instruction_pa = translated_pa;
        walk.packet->data_pa = translated_pa;
        walk.packet->translated = COMPLETED;
        walk.packet->data = translated_pa >> LOG2_PAGE_SIZE;
        walk.packet->hit_where = hit_where_t::PTW;
        
        // Return data to STLB cache so the miss can be completed
        if (stlb_cache != NULL) {
            stlb_cache->return_data(walk.packet);
        }
    }
}

// Print configuration
void PTWclass::print_config() {
    cout << "=== PTW Configuration ===" << endl;
    cout << "CPU: " << cpu << endl;
    cout << "PTW Latency: " << PTW_LATENCY << " cycles" << endl;
    cout << "Max Outstanding Walks: " << MAX_OUTSTANDING_WALKS << endl;
    cout << endl << "Page Walk Cache (PwC) Configuration:" << endl;
    cout << "Level 4 (PML4): " << PWC_L4_SETS << " sets x " << PWC_L4_WAYS << " ways" << endl;
    cout << "Level 3 (PDPT): " << PWC_L3_SETS << " sets x " << PWC_L3_WAYS << " ways" << endl;
    cout << "Level 2 (PD):   " << PWC_L2_SETS << " sets x " << PWC_L2_WAYS << " ways" << endl;
    cout << "Level 1 (PT):   " << PWC_L1_SETS << " sets x " << PWC_L1_WAYS << " ways" << endl;
    cout << endl;
}

// Print statistics
void PTWclass::print_stats() {
    cout << "=== PTW Statistics (CPU " << cpu << ") ===" << endl;
    cout << "Total Page Walks Initiated: " << stats.total_walks_initiated << endl;
    cout << "Walks Completed: " << stats.walks_completed << endl;
    cout << "Walks Stalled: " << stats.walks_stalled << endl;
    cout << "Average Walk Latency: ";
    if (stats.walks_completed > 0) {
        cout << (float)stats.total_latency / stats.walks_completed << " cycles" << endl;
    } else {
        cout << "0 cycles" << endl;
    }
    cout << "Max Walk Latency: " << stats.max_latency << " cycles" << endl;
    cout << endl << "Page Walk Cache Statistics:" << endl;
    
    cout << "Level 4 (PML4): " << level_caches[0].hits << " hits, " 
         << level_caches[0].misses << " misses";
    if ((level_caches[0].hits + level_caches[0].misses) > 0) {
        cout << " (Hit Rate: " << (float)level_caches[0].hits / (level_caches[0].hits + level_caches[0].misses) * 100.0 << "%)";
    }
    cout << endl;
    
    cout << "Level 3 (PDPT): " << level_caches[1].hits << " hits, " 
         << level_caches[1].misses << " misses";
    if ((level_caches[1].hits + level_caches[1].misses) > 0) {
        cout << " (Hit Rate: " << (float)level_caches[1].hits / (level_caches[1].hits + level_caches[1].misses) * 100.0 << "%)";
    }
    cout << endl;
    
    cout << "Level 2 (PD):   " << level_caches[2].hits << " hits, " 
         << level_caches[2].misses << " misses";
    if ((level_caches[2].hits + level_caches[2].misses) > 0) {
        cout << " (Hit Rate: " << (float)level_caches[2].hits / (level_caches[2].hits + level_caches[2].misses) * 100.0 << "%)";
    }
    cout << endl;
    
    cout << "Level 1 (PT):   " << level_caches[3].hits << " hits, " 
         << level_caches[3].misses << " misses";
    if ((level_caches[3].hits + level_caches[3].misses) > 0) {
        cout << " (Hit Rate: " << (float)level_caches[3].hits / (level_caches[3].hits + level_caches[3].misses) * 100.0 << "%)";
    }
    cout << endl;
    
    cout << endl << "Total PwC Replacements: "
         << (level_caches[0].replacements + level_caches[1].replacements + 
             level_caches[2].replacements + level_caches[3].replacements) << endl;
    cout << endl;

    // CSV output
    cout << "\n=== PTW Statistics CSV (CPU " << cpu << ") ===" << endl;
    auto printScalar = [&](const string &name, uint64_t value) {
        cout << name << "," << value << "\n";
    };
    printScalar("total_walks_initiated", stats.total_walks_initiated);
    printScalar("walks_completed", stats.walks_completed);
    printScalar("walks_stalled", stats.walks_stalled);
    printScalar("total_latency", stats.total_latency);
    printScalar("max_latency", stats.max_latency);
    printScalar("total_page_faults", stats.total_page_faults);

    vector<string> names = {"PwC","L1D","L2C","LLC","DRAM"};
    for (uint32_t lvl = 0; lvl < PWC_TOTAL_LEVELS; lvl++) {
        cout << "level" << lvl << "_hit_locations,";
        for (size_t i = 0; i < names.size(); i++) {
            cout << names[i];
            if (i+1 < names.size()) cout << ";";
        }
        cout << "\n";
        cout << "level" << lvl << "_hit_counts,";
        cout << stats.level_stats[lvl].pwc_hits << ";";
        cout << stats.level_stats[lvl].l1d_hits << ";";
        cout << stats.level_stats[lvl].l2c_hits << ";";
        cout << stats.level_stats[lvl].llc_hits << ";";
        cout << stats.level_stats[lvl].dram_hits << "\n";
    }
}
