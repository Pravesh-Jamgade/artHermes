#include "ptw.h"
#include "logging.h"
#include "champsim.h"
#include "ooo_cpu.h"
#include "cache.h"
#include "stlb_ptw_integration.h"
#include "uncore.h"
#include "buddy_allocator.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include "const.h"

static const uint64_t CR3_STRIDE_100GB = (100ULL * 1024ULL * 1024ULL * 1024ULL);

using namespace std;

// Constructor
PTWclass::PTWclass(uint32_t cpu_id) 
    : cpu(cpu_id),
      PTW_LATENCY(100),
      MAX_OUTSTANDING_WALKS(16),
      stlb_cache(NULL),
      dtlb_cache(NULL),
      itlb_cache(NULL)
{
    // explicitly initialize array elements since they cannot be in the init list
    level_caches[0] = PageWalkCacheLevel(PWC_L4_SETS, PWC_L4_WAYS);
    level_caches[1] = PageWalkCacheLevel(PWC_L3_SETS, PWC_L3_WAYS);
    level_caches[2] = PageWalkCacheLevel(PWC_L2_SETS, PWC_L2_WAYS);
    level_caches[3] = PageWalkCacheLevel(PWC_L1_SETS, PWC_L1_WAYS);

    memset(&stats, 0, sizeof(stats));
    initialize();
}

// Destructor
PTWclass::~PTWclass() {
    print_stats();
}

// Initialize PTW
void PTWclass::initialize() {
    outstanding_walks.clear();

    // Reset MSHR
    for (int i = 0; i < PTW_MSHR_SIZE; i++)
        mshr[i] = PTW_MSHR_Entry();

    // Initialize per-CPU CR3 bases with 100GB spacing
    for (uint32_t i = 0; i < NUM_CPUS; i++) {
        cr3_base_addrs[i] = (uint64_t)(i + 1) * CR3_STRIDE_100GB;
    }
    
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
uint64_t PTWclass::get_level_index(uint64_t vaddr, uint32_t level) {
    uint64_t index_bits = (vaddr >> (LOG2_PAGE_SIZE + (level * 9))) & ((1ULL << 9) - 1);
    return index_bits;
}

// Get set index for a PwC level
uint64_t PTWclass::get_level_set(uint64_t curr_pa, uint32_t level) {
    if (level_caches[level].sets <= 1) {
        return 0;
    }
    uint64_t set_index = (curr_pa >> 3) & ((1ULL << lg2(level_caches[level].sets)) - 1);
    return set_index;
}

// Extract virtual address tag for a level
uint64_t PTWclass::get_level_tag(uint64_t curr_pa, uint32_t level) {
    uint64_t block_addr = curr_pa >> 3; // block offset is 3 bits (8 bytes)
    if (level_caches[level].sets <= 1) {
        return block_addr;
    }
    uint64_t set_bits = lg2(level_caches[level].sets);
    return (block_addr >> set_bits);
}

// Lookup in PwC at a specific level
bool PTWclass::pwc_lookup(uint64_t curr_pa, uint32_t level, uint64_t &paddr) {
    if (level >= PWC_TOTAL_LEVELS) return false;
    
    uint64_t set = get_level_set(curr_pa, level);
    uint64_t tag = get_level_tag(curr_pa, level);
    
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
void PTWclass::pwc_insert(uint64_t curr_addr, uint64_t paddr, uint32_t level) {
    if (level >= PWC_TOTAL_LEVELS) return;
    
    uint32_t set = get_level_set(curr_addr, level);
    uint64_t tag = get_level_tag(curr_addr, level);
    
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
    victim.vaddr = curr_addr;
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
    
    uint32_t set = get_level_set(vaddr, level);
    uint64_t tag = get_level_tag(vaddr, level);
    
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
        LOG_PTW("PTW[%u]: Walk queue full, cannot initiate walk for vaddr=0x%lx", cpu, vaddr);
        return; // Queue is full
    }
    
    OutstandingWalk new_walk;
    new_walk.vaddr = vaddr;
    new_walk.current_level = 3; // Start from L4 (PML4)
    new_walk.current_pa = cr3_base_addrs[cpu]; // initial PA is CR3 base, we will add level offsets in operate()
    new_walk.requested_cycle = current_core_cycle[cpu];
    new_walk.instr_id = packet->instr_id;
    new_walk.packet = *packet;  // copy by value — packet ptr may become stale before walk completes
    LOG_PTW("PTW[%u]: Initiate walk for vaddr=0x%lx instr_id=%u", cpu, vaddr, packet->instr_id);
    outstanding_walks.push_back(new_walk);
    stats.total_walks_initiated++;

    l.log("PTWInitiate", hex2str(packet->address), hex2str(packet->full_addr), " instr=", packet->instr_id, "ins=", +packet->instruction, '\n');
}

// Check if walk queue is available
bool PTWclass::is_walk_queue_available() {
    return outstanding_walks.size() < MAX_OUTSTANDING_WALKS;
}

// Operate PTW (process outstanding walks)
void PTWclass::operate() {
    vector<size_t> to_remove;

    for (size_t i = 0; i < outstanding_walks.size(); i++) {
        OutstandingWalk &walk = outstanding_walks[i];
        bool dispatched = false;

        // Advance the walk through PwC hits; stop on first PwC miss (sends to memory)
        while (walk.current_level >= 0) {
            uint32_t curr_lvl = (uint32_t)walk.current_level;

            // PTE byte address = table base + level-specific index * 8
            uint64_t pte_addr = walk.current_pa
                                + get_level_index(walk.vaddr, curr_lvl) * sizeof(uint64_t);

            uint64_t pa;
            if (pwc_lookup(pte_addr, curr_lvl, pa)) {
                walk.level_stats[curr_lvl].access_count++;
                stats.level_stats[curr_lvl].access_count++;
                // PwC hit: pa = next-level table base
                l.log("PwCHit", " lvl=", curr_lvl, " vaddr=", hex2str(walk.vaddr),
                       " pte=", hex2str(pte_addr), " -> ", hex2str(pa), " instr=", walk.instr_id, "ins=", +walk.packet.instruction, '\n');
                walk.level_stats[curr_lvl].hit_count++;
                walk.level_stats[curr_lvl].hit_where = 0; // PwC
                stats.level_stats[curr_lvl].hit_count++;
                stats.level_stats[curr_lvl].pwc_hits++;

                walk.current_pa = pa;    // advance to next-level table base
                walk.current_level--;
                continue;                // try next level in same cycle
            }

            int mshr_idx = find_free_mshr();
            if (mshr_idx == -1) {
                stats.walks_stalled++;
                break; // MSHR full, retry next cycle
            }

            // Check if another MSHR entry is already waiting for the same cache block
            // at the same level.  If so, skip the L1D dispatch (L1D would merge and
            // silently drop the second ptw_walk_ptr, causing a deadlock).  Mark this
            // entry as a piggyback — handle_memory_response will re-queue it as an
            // outstanding walk once the primary response arrives and the PwC is filled.
            bool already_in_flight = false;
            for (int mi = 0; mi < PTW_MSHR_SIZE; mi++) {
                if (mshr[mi].valid && !mshr[mi].piggyback
                    && mshr[mi].current_pa >> LOG2_BLOCK_SIZE == pte_addr >> LOG2_BLOCK_SIZE
                    && mshr[mi].current_level == (int)curr_lvl) {
                    already_in_flight = true;
                    break;
                }
            }

            if (!already_in_flight) {
                // First walk for this cache block: dispatch a new TRANSLATION to L1D.
                PACKET req;
                memset(&req, 0, sizeof(req));
                req.address      = pte_addr >> LOG2_BLOCK_SIZE;
                req.full_addr    = pte_addr;
                req.virt_addr    = walk.vaddr;
                req.cpu          = cpu;
                req.is_data      = 1;
                req.type         = TRANSLATION;
                req.instruction  = 0;
                req.tlb_access   = 1;
                req.fill_level   = FILL_L1;
                req.fill_l1d     = 1;
                req.from_ptw     = 1;
                req.ptw_level    = curr_lvl;
                req.ptw_walk_ptr = (void *)&mshr[mshr_idx];

                int status = ooo_cpu[cpu].L1D.add_rq(&req);
                if (status == -2) {
                    stats.walks_stalled++;
                    break; // L1D RQ full, retry next cycle
                }
            }

            // Register this walk in the PTW MSHR.
            mshr[mshr_idx].valid           = true;
            mshr[mshr_idx].piggyback       = already_in_flight;
            mshr[mshr_idx].vaddr           = walk.vaddr;
            mshr[mshr_idx].current_level   = (int)curr_lvl;
            mshr[mshr_idx].current_pa      = pte_addr;       // PwC insert key on response
            mshr[mshr_idx].table_base_pa   = walk.current_pa; // table base for re-queue
            mshr[mshr_idx].requested_cycle = walk.requested_cycle;
            mshr[mshr_idx].instr_id        = walk.instr_id;
            mshr[mshr_idx].packet          = walk.packet;
            for (uint32_t lv = 0; lv < PWC_TOTAL_LEVELS; lv++)
                mshr[mshr_idx].level_stats[lv] = walk.level_stats[lv];
            mshr[mshr_idx].total_page_faults = walk.total_page_faults;

            l.log("PTWDisp", " lvl=", curr_lvl, " vaddr=", hex2str(walk.vaddr),
                   " pte=", hex2str(pte_addr), " piggyback=", already_in_flight,
                   " instr=", walk.instr_id, "ins=", +walk.packet.instruction, '\n');
            dispatched = true;

            // PwC miss: dispatch to memory via MSHR
            walk.level_stats[curr_lvl].miss_count++;
            stats.level_stats[curr_lvl].miss_count++;

            walk.level_stats[curr_lvl].access_count++;
            stats.level_stats[curr_lvl].access_count++;

            // initalizing page tracking with all PTE set to page_fault = true.  
            // The first walk to miss on this PTE will record the page fault in stats, 
            // and subsequent walks will see page_fault = false and not double-count.
            buddy_allocator.shadow_init_page(pte_addr >> LOG2_PAGE_SIZE, curr_lvl);

            // Free lookup: check whether this specific PTE entry is being
            // walked for the first time (page_fault == true).  If so, record
            // the page fault in per-level stats 
            {
                uint64_t shadow_val;
                bool is_pf;
                if (buddy_allocator.shadow_get_entry(pte_addr, curr_lvl, shadow_val, is_pf) && is_pf) {
                    walk.level_stats[curr_lvl].page_fault = 1;
                    stats.level_stats[curr_lvl].page_faults++;
                    stats.total_page_faults++;
                }
            }
            
            break; // walk is now in MSHR; remove from outstanding
        }

        // If all levels resolved via PwC (no memory needed), complete immediately
        if (!dispatched && walk.current_level < 0) {
            l.log("PTWComplete_PwC", hex2str(walk.vaddr), hex2str(walk.current_pa), " instr=", walk.instr_id, "ins=", +walk.packet.instruction, '\n');
            complete_page_walk(walk.packet, walk.current_pa);
            stats.walks_completed++;
            uint64_t latency = current_core_cycle[cpu] - walk.requested_cycle;
            stats.total_latency += latency;
            if (latency > stats.max_latency) stats.max_latency = latency;
            dispatched = true; // also remove
        }

        if (dispatched)
            to_remove.push_back(i);
    }

    // Remove dispatched / completed walks in reverse order to preserve indices
    for (int j = (int)to_remove.size() - 1; j >= 0; j--)
        outstanding_walks.erase(outstanding_walks.begin() + to_remove[j]);
}

// handle memory response for a previously issued PTE request
void PTWclass::handle_memory_response(PACKET *packet) {
    if (packet == NULL) return;
    if (!packet->from_ptw) return;

    PTW_MSHR_Entry *me = (PTW_MSHR_Entry *)packet->ptw_walk_ptr;
    if (me == NULL || !me->valid) {
        LOG_PTW("PTW[%u]: handle_memory_response: NULL or invalid MSHR ptr", cpu);
        return;
    }

    uint32_t lvl             = packet->ptw_level;
    uint64_t next_level_base = packet->data; // PTE value = next-level table base (or phys addr)

    // Track where the data came from
    uint8_t hit_src = 0;
    if      (packet->hit_where == hit_where_t::L1D) { hit_src = 1; stats.level_stats[lvl].l1d_hits++; }
    else if (packet->hit_where == hit_where_t::L1I) { hit_src = 2; stats.level_stats[lvl].l1i_hits++; }
    else if (packet->hit_where == hit_where_t::L2C) { hit_src = 3; stats.level_stats[lvl].l2c_hits++; }
    else if (packet->hit_where == hit_where_t::LLC) { hit_src = 4; stats.level_stats[lvl].llc_hits++; }
    else if (packet->hit_where == hit_where_t::DRAM){ hit_src = 5; stats.level_stats[lvl].dram_hits++;}
    me->level_stats[lvl].hit_where = hit_src;

    l.log("PTWResp", " lvl=", lvl, " vaddr=", hex2str(me->vaddr),
           " pte=", hex2str(me->current_pa), " -> ", hex2str(next_level_base), " instr=", me->instr_id, "ins=", +me->packet.instruction, '\n');

    // Insert into PwC: key = PTE byte address, value = next-level table base
    pwc_insert(me->current_pa, next_level_base, lvl);

    int next_level = (int)lvl - 1;

    if (next_level >= 0) {
        // More levels to walk: push a new OutstandingWalk for the next level
        OutstandingWalk nw;
        nw.vaddr           = me->vaddr;
        nw.current_level   = next_level;
        nw.current_pa      = next_level_base; // table base for next level
        nw.requested_cycle = me->requested_cycle;
        nw.instr_id        = me->instr_id;
        nw.packet          = me->packet;
        for (uint32_t lv = 0; lv < PWC_TOTAL_LEVELS; lv++)
            nw.level_stats[lv] = me->level_stats[lv];
        nw.total_page_faults = me->total_page_faults;
        outstanding_walks.push_back(nw);
    } 
    else {
        // All levels done: complete the walk
        l.log("PTWComplete", hex2str(me->vaddr), hex2str(next_level_base), " instr=", me->instr_id, "ins=", +me->packet.instruction, '\n');
        complete_page_walk(me->packet, next_level_base);
        stats.walks_completed++;
        uint64_t latency = current_core_cycle[cpu] - me->requested_cycle;
        stats.total_latency += latency;
        if (latency > stats.max_latency) stats.max_latency = latency;
    }

    // Free the MSHR slot
    me->valid = false;

    // Re-queue any piggyback MSHR entries that were waiting for the same
    // (pte cache block, level).  The PwC now has the answer, so re-queuing
    // them as OutstandingWalks at the same level causes an immediate PwC hit
    // on the next operate() cycle — no extra L1D request needed.
    uint64_t primary_pte_addr = me->current_pa; // already freed but value is still valid
    for (int i = 0; i < PTW_MSHR_SIZE; i++) {
        PTW_MSHR_Entry &pb = mshr[i];
        if (!pb.valid || !pb.piggyback) continue;
        if ((uint32_t)pb.current_level != lvl) continue;
        if (pb.current_pa >> LOG2_BLOCK_SIZE != primary_pte_addr >> LOG2_BLOCK_SIZE) continue;

        // Re-queue at the same level with the original table base so operate()
        // re-computes pte_addr and hits the PwC entry we just inserted.
        OutstandingWalk nw;
        nw.vaddr           = pb.vaddr;
        nw.current_level   = (int)lvl;   // same level — will PwC-hit immediately
        nw.current_pa      = pb.table_base_pa;
        nw.requested_cycle = pb.requested_cycle;
        nw.instr_id        = pb.instr_id;
        nw.packet          = pb.packet;
        for (uint32_t lv2 = 0; lv2 < PWC_TOTAL_LEVELS; lv2++)
            nw.level_stats[lv2] = pb.level_stats[lv2];
        nw.total_page_faults = pb.total_page_faults;
        outstanding_walks.push_back(nw);

        pb.valid = false;
    }
}

// Complete a page walk — fill result into STLB packet and return to STLB
void PTWclass::complete_page_walk(PACKET &pkt, uint64_t translated_pa) {
    LOG_PTW("PTW[%u]: complete_page_walk pa=0x%lx", cpu, translated_pa);
    pkt.instruction_pa = translated_pa;
    pkt.data_pa        = translated_pa;
    pkt.translated     = COMPLETED;
    pkt.data           = translated_pa >> LOG2_PAGE_SIZE;
    pkt.hit_where      = hit_where_t::PTW;
    if (stlb_cache != NULL)
        stlb_cache->return_data(&pkt);
}

// Find a free MSHR slot; returns index or -1 if all slots are occupied
int PTWclass::find_free_mshr() {
    for (int i = 0; i < PTW_MSHR_SIZE; i++) {
        if (!mshr[i].valid)
            return i;
    }
    return -1;
}

// Print configuration
void PTWclass::print_config() {
}

// Print statistics
void PTWclass::print_stats() {
    auto printScalar = [&](const string &name, uint64_t value) {
        cout << "PTW," << cpu << "," << name << "," << value << "\n";
    };
    auto printFloat = [&](const string &name, double value) {
        cout << "PTW," << cpu << "," << name << "," << fixed << setprecision(2) << value << "\n";
    };

    printScalar("total_walks_initiated", stats.total_walks_initiated);
    printScalar("walks_completed",       stats.walks_completed);
    printScalar("walks_stalled",         stats.walks_stalled);
    printScalar("total_latency",         stats.total_latency);
    printScalar("max_latency",           stats.max_latency);
    printScalar("total_page_faults",     stats.total_page_faults);

    const char *lvl_names[] = {"PML4", "PDPT", "PD", "PT"};
    for (uint32_t lvl = 0; lvl < PWC_TOTAL_LEVELS; lvl++) {
        string pfx = string("level") + lvl_names[PWC_TOTAL_LEVELS - 1 - lvl] + "_";

        uint64_t accesses = stats.level_stats[lvl].access_count;
        uint64_t pwc_hits = stats.level_stats[lvl].pwc_hits;
        uint64_t misses   = stats.level_stats[lvl].miss_count;
        uint64_t pf       = stats.level_stats[lvl].page_faults;

        printScalar(pfx + "accesses",    accesses);
        printScalar(pfx + "pwc_hits",    pwc_hits);
        printScalar(pfx + "misses",      misses);
        printScalar(pfx + "page_faults", pf);

        // PwC hit rate (%) over all accesses at this level
        double pwc_hit_rate = (accesses > 0) ? (100.0 * pwc_hits / accesses) : 0.0;
        printFloat(pfx + "pwc_hit_rate_pct", pwc_hit_rate);

        // page-fault rate (%) over misses that went to memory
        double pf_rate = (misses > 0) ? (100.0 * pf / misses) : 0.0;
        printFloat(pfx + "page_fault_rate_pct", pf_rate);

        // Single-row hits: PTW,<cpu>,level<LVL>_hits,pwc,l1d,l1i,l2c,llc,dram
        cout << "PTW," << cpu << "," << pfx << "hits, pwc, l1d, l1i, l2c, llc, dram\n";
        cout << "PTW," << cpu << "," << pfx << "hits"
             << "," << pwc_hits
             << "," << stats.level_stats[lvl].l1d_hits
             << "," << stats.level_stats[lvl].l1i_hits
             << "," << stats.level_stats[lvl].l2c_hits
             << "," << stats.level_stats[lvl].llc_hits
             << "," << stats.level_stats[lvl].dram_hits
             << "\n";
    }
}
