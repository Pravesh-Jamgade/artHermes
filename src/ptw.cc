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
#include <set>
#include "const.h"
#include "offchip_pred_base.h"
#include "offchip_pred_factory.h"

static const uint64_t CR3_STRIDE_10GB = (10ULL * 1024ULL * 1024ULL * 1024ULL);

using namespace std;

namespace knob{
    extern bool enable_translation_ocp;
    extern bool enable_oracle_ptw_pred;
    extern int  bw_parallel_walk;
};

// Constructor
PTWclass::PTWclass(uint32_t cpu_id) 
    : cpu(cpu_id),
      stlb_cache(NULL),
      dtlb_cache(NULL),
      itlb_cache(NULL),
      l1d(NULL),
      l2(NULL),
      llc(NULL),
      PTW_LATENCY(100),
      PTW_RQ_LATENCY(2)
{
    // explicitly initialize array elements since they cannot be in the init list
    level_caches[0] = PageWalkCacheLevel(PWC_L4_SETS, PWC_L4_WAYS);
    level_caches[1] = PageWalkCacheLevel(PWC_L3_SETS, PWC_L3_WAYS);
    level_caches[2] = PageWalkCacheLevel(PWC_L2_SETS, PWC_L2_WAYS);
    level_caches[3] = PageWalkCacheLevel(PWC_L1_SETS, PWC_L1_WAYS);

    memset(&stats, 0, sizeof(stats));

    MAX_OUTSTANDING_WALKS = knob::bw_parallel_walk;

    // Instantiate off-chip predictor using Factory Pattern
    std::string pred_type = knob::enable_oracle_ptw_pred ? "oracle" : "perc";
    trans_offchip_pred = OffchipPredFactory::create_predictor(pred_type, cpu, champsim_seed);

    initialize();
}

// Destructor
PTWclass::~PTWclass() {
    if (trans_offchip_pred != nullptr) {
        trans_offchip_pred->dump_stats();
        delete trans_offchip_pred;
    }
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
        cr3_base_addrs[i] = (uint64_t)(i + 1) * CR3_STRIDE_10GB;
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
bool PTWclass::initiate_page_walk(PACKET *packet, uint64_t vaddr) {
    if (!is_walk_queue_available(vaddr)) {
        l.log("PTWStall, vaddr", hex2str(vaddr), "addr", hex2str(packet->address), " instr", packet->instr_id, "ins", +packet->instruction, "current-cy", current_core_cycle[cpu], '\n');
        add_history_event(current_core_cycle[cpu], packet->instr_id, vaddr >> LOG2_BLOCK_SIZE, vaddr, TRANSLATION, "PTW_STALL", "PTW", false, false, false, false, 0, 0, 3);
        return false; // Queue is full
    }

    OutstandingWalk new_walk;
    new_walk.event_cycle = current_core_cycle[cpu] + PTW_RQ_LATENCY; // for stats, not used in logic since we operate on all walks every cycle
    new_walk.virt_full_addr = vaddr;
    new_walk.current_level = 3; // Start from L4 (PML4)
    new_walk.phy_full_addr = cr3_base_addrs[cpu]; // initial PA is CR3 base, we will add level offsets in operate()
    new_walk.requested_cycle = current_core_cycle[cpu];
    new_walk.instr_id = packet->instr_id;
    new_walk.packet = *packet;  // copy by value — packet ptr may become stale before walk completes
    outstanding_walks.push_back(new_walk);
    stats.total_walks_initiated++;

    // if (stats.total_walks_initiated <= 5)
    //     cerr << "[PTW_DIAG] initiate_page_walk #" << stats.total_walks_initiated
    //          << " vaddr=" << hex2str(vaddr)
    //          << " instr_id=" << packet->instr_id
    //          << " cycle=" << current_core_cycle[cpu] << endl;

    l.log("PTWInitiate, vaddr", hex2str(vaddr),"addr", hex2str(packet->address), " instr", packet->instr_id, "level", new_walk.current_level, "base_ptaddr", hex2str(new_walk.phy_full_addr), "ins", +packet->instruction, "current-cy", current_core_cycle[cpu], '\n');
    add_history_event(current_core_cycle[cpu], packet->instr_id, vaddr >> LOG2_BLOCK_SIZE, vaddr, TRANSLATION, "PTW_INIT", "PTW", false, false, false, false, 0, 0, 3);
    return true;
}

// Check if walk queue is available
bool PTWclass::is_walk_queue_available(uint64_t vaddr) {
    if (outstanding_walks.size() >= MAX_OUTSTANDING_WALKS) {
        return false;
    }
    return true;
}

// Operate PTW (process outstanding walks)
void PTWclass::operate() {
    vector<size_t> to_remove;

    for (size_t i = 0; i < outstanding_walks.size(); i++) {
        OutstandingWalk &walk = outstanding_walks[i];
        if (walk.event_cycle > current_core_cycle[cpu]) {
            continue; // Not ready yet, respect PTW request queue latency
        }
        bool dispatched = false;

        // Advance the walk through PwC hits; stop on first PwC miss (sends to memory)
        while (walk.current_level >= 0) {
            uint32_t curr_lvl = (uint32_t)walk.current_level;

            // PTE byte address = table base + level-specific index * 8
            uint64_t pte_addr = walk.phy_full_addr
                                + get_level_index(walk.virt_full_addr, curr_lvl) * sizeof(uint64_t);
            
            uint64_t pa;
            if (pwc_lookup(pte_addr, curr_lvl, pa)) {

                l.log("PTW_PwC_Hit, vaddr", hex2str(walk.virt_full_addr),"paddr", hex2str(walk.phy_full_addr), "level", curr_lvl, "pte_addr", hex2str(pte_addr), "next_pt", hex2str(pa), "instr", walk.instr_id, "ins", +walk.packet.instruction, "current-cy", current_core_cycle[cpu], '\n');
                add_history_event(current_core_cycle[cpu], walk.instr_id, pte_addr >> LOG2_BLOCK_SIZE, pte_addr, TRANSLATION, "PTW_PWC_HIT", "PTW", false, false, false, false, 0, 0, curr_lvl);
                walk.level_stats[curr_lvl].access_count++;
                stats.level_stats[curr_lvl].access_count++;
                // PwC hit: pa = next-level table base
                walk.level_stats[curr_lvl].hit_count++;
                walk.level_stats[curr_lvl].hit_where = 0; // PwC
                stats.level_stats[curr_lvl].hit_count++;
                stats.level_stats[curr_lvl].pwc_hits++;

                walk.phy_full_addr = pa;    // advance to next-level table base
                walk.current_level--;
                continue;                // try next level in same cycle
            }

            int mshr_idx = find_free_mshr();
            if (mshr_idx == -1) {
                stats.walks_stalled++;
                l.log("PTW_PwC_Miss_MSHRFull, vaddr", hex2str(walk.virt_full_addr),"paddr", hex2str(walk.phy_full_addr), "level", curr_lvl, "pte_addr", hex2str(pte_addr), "instr", walk.instr_id, "ins", +walk.packet.instruction, "current-cy", current_core_cycle[cpu], '\n');
                add_history_event(current_core_cycle[cpu], walk.instr_id, pte_addr >> LOG2_BLOCK_SIZE, pte_addr, TRANSLATION, "PTW_MSHR_STALL", "PTW", false, false, false, false, 0, 0, curr_lvl);
                return; // MSHR full, retry next cycle
            }

            // Check if another MSHR entry is already waiting for the same cache block
            // at the same level.  If so, skip the L1D dispatch (L1D would merge and
            // silently drop the second ptw_walk_ptr, causing a deadlock).  Mark this
            // entry as a piggyback — handle_memory_response will re-queue it as an
            // outstanding walk once the primary response arrives and the PwC is filled.
            bool already_in_flight = false;
            int match_mshr_idx = -1;
            for (int mi = 0; mi < PTW_MSHR_SIZE; mi++) {
                if (mshr[mi].valid
                    && mshr[mi].current_pte_addr >> LOG2_BLOCK_SIZE == pte_addr >> LOG2_BLOCK_SIZE
                    && mshr[mi].current_level == (int)curr_lvl) {
                    already_in_flight = true;
                    match_mshr_idx = mi;
                    PTW_MSHR_Entry* me = &mshr[mi];
                    debug.log(
                        "Duplicate", "instr1", me->instr_id, "full_vaddr", hex2str(me->vaddr), "pte_addr_full", hex2str(me->current_pte_addr), "lvl", me->current_level,
                        "instr0", walk.instr_id, "full_vaddr", hex2str(walk.virt_full_addr), "pte_addr_full", hex2str(pte_addr), "lvl", curr_lvl, '\n'
                    );

                    // Do NOT mark the existing primary entry as piggyback.
                    // mshr[mshr_idx].piggyback = already_in_flight (line below) marks the NEW walk.
                    break;
                }
            }

            PACKET req;
            memset(&req, 0, sizeof(req));
            req.address      = pte_addr >> LOG2_BLOCK_SIZE;
            req.full_addr    = pte_addr;
            req.virt_addr    = walk.virt_full_addr;
            req.cpu          = cpu;
            req.is_data      = 1;
            req.type         = TRANSLATION;
            req.instruction  = 0;
            req.tlb_access   = 1;
            req.fill_level   = FILL_L1;
            req.fill_l1d     = 1;
            req.from_ptw     = 1;
            req.ptw_level    = curr_lvl;
            req.instr_id     = walk.instr_id;
            req.ptw_walk_ptr = (void *)&mshr[mshr_idx];
            
            // required by perceptron 
            req.ip            = walk.packet.ip;
            req.data_index    = walk.packet.data_index;
            req.rob_index     = walk.packet.rob_index;
            req.lq_index      = walk.packet.lq_index;
            req.pwc_miss_mem_hitwhere = walk.packet.pwc_miss_mem_hitwhere; // to set prev-walk on/off-chip features for perceptron

            trans_offchip_pred->set_state_info(&req); // set all necessary info for predictor to make decision and for training on response

            if (!already_in_flight) {

                // Now only for original requests we are generating prediction
                trans_offchip_pred->predict(&req); // populate req.ocp_feature and req.went_offchip_pred

                // First walk for this cache block: dispatch a new TRANSLATION to L1D.
                if(req.went_offchip_pred && knob::enable_translation_ocp)
                {
                    PACKET offchip_req = req; // copy by value since req will be modified for on-chip dispatch below
                    offchip_req.fill_level = FILL_DDRP;
                    offchip_req.fill_l1d = 0;
                    offchip_req.type = PREFETCH;
                    offchip_req.went_offchip_pred = req.went_offchip_pred;

                    stats.pred_stats.ddrp_offchip_calls++;
                    if(ooo_cpu[cpu].dram_controller->get_occupancy(1, offchip_req.full_addr >> LOG2_BLOCK_SIZE) == ooo_cpu[cpu].dram_controller->get_size(1, offchip_req.full_addr >> LOG2_BLOCK_SIZE))
                    {
                        stats.pred_stats.ddrp_offchip_stalled++;
                    }
                    else 
                    {
                        stats.pred_stats.ddrp_offchip_sent++;
                        ooo_cpu[cpu].dram_controller->add_rq(&offchip_req);
                    }
                }

                int status = ooo_cpu[cpu].L1D.add_rq(&req);
                if (status == -2) {
                    stats.walks_stalled++;
                    l.log("PTW_PwC_Miss_L1DFull, vaddr", hex2str(walk.virt_full_addr),"paddr", hex2str(walk.phy_full_addr), "level", curr_lvl, "pte_addr", hex2str(pte_addr), "instr", walk.instr_id, "ins", +walk.packet.instruction, "current-cy", current_core_cycle[cpu], '\n');
                    add_history_event(current_core_cycle[cpu], walk.instr_id, pte_addr >> LOG2_BLOCK_SIZE, pte_addr, TRANSLATION, "PTW_L1D_STALL", "PTW", false, false, false, false, 0, 0, curr_lvl);
                    break; // L1D RQ full, retry next cycle
                }
            }
            else{
                if (match_mshr_idx != -1) {
                    req.went_offchip_pred = mshr[match_mshr_idx].feature_packet.went_offchip_pred;
                }
            }

            // Register this walk in the PTW MSHR.
            mshr[mshr_idx].valid           = true;
            mshr[mshr_idx].piggyback       = already_in_flight;
            mshr[mshr_idx].vaddr           = walk.virt_full_addr;
            mshr[mshr_idx].current_level   = (int)curr_lvl;
            mshr[mshr_idx].current_pte_addr = pte_addr;       // PwC insert key on response
            mshr[mshr_idx].table_base_pa   = walk.phy_full_addr; // table base for re-queue
            mshr[mshr_idx].requested_cycle = walk.requested_cycle;
            mshr[mshr_idx].instr_id        = walk.instr_id;
            mshr[mshr_idx].packet          = walk.packet;
            mshr[mshr_idx].feature_packet     = req;

            for (uint32_t lv = 0; lv < PWC_TOTAL_LEVELS; lv++)
                mshr[mshr_idx].level_stats[lv] = walk.level_stats[lv];
            mshr[mshr_idx].total_page_faults = walk.total_page_faults;
            dispatched = true;

            l.log("PTW_PwC_Miss_Sent, vaddr", hex2str(walk.virt_full_addr),"paddr", hex2str(walk.phy_full_addr), "level", curr_lvl, "pte_addr", hex2str(pte_addr), "already_in_flight", already_in_flight, "instr", walk.instr_id, "ins", +walk.packet.instruction, "current-cy", current_core_cycle[cpu], '\n');
            if (already_in_flight) {
                add_history_event(current_core_cycle[cpu], walk.instr_id, pte_addr >> LOG2_BLOCK_SIZE, pte_addr, TRANSLATION, "PTW_MISS_MERGE", "PTW", false, false, false, true, (match_mshr_idx != -1 ? mshr[match_mshr_idx].instr_id : 0), 0, curr_lvl);
            } else {
                add_history_event(current_core_cycle[cpu], walk.instr_id, pte_addr >> LOG2_BLOCK_SIZE, pte_addr, TRANSLATION, "PTW_MISS_SENT", "PTW", false, false, false, false, 0, 0, curr_lvl);
            }

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
            complete_page_walk(walk.packet, walk.phy_full_addr);
            stats.walks_completed++;
            uint64_t latency = current_core_cycle[cpu] - walk.requested_cycle;
            stats.total_latency += latency;
            if (latency > stats.max_latency) stats.max_latency = latency;
            dispatched = true; // also remove
            l.log("PTWComplete_PwC, vaddr", hex2str(walk.virt_full_addr),"paddr", hex2str(walk.phy_full_addr), "level", walk.current_level, "pte_val", hex2str(walk.phy_full_addr), "instr", walk.instr_id, "ins", +walk.packet.instruction, "current-cy", current_core_cycle[cpu], '\n');
            add_history_event(current_core_cycle[cpu], walk.instr_id, walk.phy_full_addr >> LOG2_BLOCK_SIZE, walk.phy_full_addr, TRANSLATION, "PTW_COMPLETE_PWC", "PTW", false, false, false, false, 0, 0, walk.current_level);
        }

        if (dispatched)
            to_remove.push_back(i);
    }

    // Remove dispatched / completed walks in reverse order to preserve indices
    for (int j = (int)to_remove.size() - 1; j >= 0; j--)
    {
        l.log("PTWRemoveOutstanding, vaddr", hex2str(outstanding_walks[to_remove[j]].virt_full_addr), "level", outstanding_walks[to_remove[j]].current_level, "base_pt", hex2str(outstanding_walks[to_remove[j]].phy_full_addr), "instr", outstanding_walks[to_remove[j]].instr_id, "ins", +outstanding_walks[to_remove[j]].packet.instruction, "current-cy", current_core_cycle[cpu], '\n');
        outstanding_walks.erase(outstanding_walks.begin() + to_remove[j]);
    }
}

// handle memory response for a previously issued PTE request
void PTWclass::handle_memory_response(PACKET *packet) {
    if (packet == NULL) return;
    if (!packet->from_ptw) return;

    uint32_t lvl             = packet->ptw_level;
    uint64_t next_level_base = packet->data; // PTE value = next-level table base (or phys addr)

    // Track where the data came from (use range checks to catch RQ/WQ/MSHR variants)
    uint8_t hit_src = 0;
    if      (packet->hit_where >= hit_where_t::L1D && packet->hit_where <= hit_where_t::L1D_MSHR) { hit_src = hit_where_t::L1D; stats.level_stats[lvl].l1d_hits++; }
    else if (packet->hit_where >= hit_where_t::L1I && packet->hit_where <= hit_where_t::L1I_MSHR) { hit_src = hit_where_t::L1I; stats.level_stats[lvl].l1i_hits++; }
    else if (packet->hit_where >= hit_where_t::L2C && packet->hit_where <= hit_where_t::L2C_MSHR) { hit_src = hit_where_t::L2C; stats.level_stats[lvl].l2c_hits++; }
    else if (packet->hit_where >= hit_where_t::LLC && packet->hit_where <= hit_where_t::LLC_MSHR) { hit_src = hit_where_t::LLC; stats.level_stats[lvl].llc_hits++; }
    else if (packet->hit_where == hit_where_t::DRAM)                                             { hit_src = hit_where_t::DRAM; stats.level_stats[lvl].dram_hits++; }
    
    // train
    trans_offchip_pred->train(packet);

    if (lvl >= 0) {

        // More levels to walk: push a new OutstandingWalk for the next level.
        // Use byte-level matching (full_addr == current_pte_addr) so two PTEs in the same
        // 64-byte cache block do not both consume the same DRAM response.
        for(int i = 0; i < PTW_MSHR_SIZE; i++) 
        {
            PTW_MSHR_Entry* me = &mshr[i];
            if (!me->valid
                || me->current_level != (int)lvl
                || packet->address != me->current_pte_addr >> LOG2_BLOCK_SIZE)
                continue;
            
            // merged accesses will also contribute to training
            if(me->piggyback)
            {
                // this is merged load, but kept separate mshr entry.
                // the actual loads went_offchip value is used for traning
                me->feature_packet.went_offchip = packet->went_offchip;
                trans_offchip_pred->train(&(me->feature_packet));
                debug.log("Return", "instr1", me->instr_id, "full_vaddr", hex2str(me->vaddr), "pte_addr_full", hex2str(me->current_pte_addr), "lvl", me->current_level, "hit_src", hit_src,
                          "instr2", packet->instr_id, "full_vaddr", hex2str(packet->virt_addr), "pte_addr_full", hex2str(packet->full_addr), "lvl", packet->ptw_level, '\n');
            }

            me->level_stats[lvl].hit_where = hit_src;
            me->packet.pwc_miss_mem_hitwhere |= ((uint32_t)hit_src << (lvl * 5));

            // Insert into PwC: key = PTE byte address, value = next-level table base
            pwc_insert(me->current_pte_addr, next_level_base, lvl);

            int next_level = (int)lvl - 1;
            
            // request next level walk
            if(next_level >= 0) {
                OutstandingWalk nw;
                nw.virt_full_addr  = me->vaddr;
                nw.current_level   = next_level;
                nw.phy_full_addr   = next_level_base; // table base for next level
                nw.requested_cycle = me->requested_cycle;
                nw.instr_id        = me->instr_id;
                nw.packet          = me->packet;
                for (uint32_t lv = 0; lv < PWC_TOTAL_LEVELS; lv++)
                    nw.level_stats[lv] = me->level_stats[lv];
                nw.total_page_faults = me->total_page_faults;
                outstanding_walks.push_back(nw);
                l.log("PTWNextLevel, vaddr", hex2str(nw.virt_full_addr),"paddr", hex2str(nw.phy_full_addr), "level", next_level, "instr", nw.instr_id, "ins", +nw.packet.instruction, "current-cy", current_core_cycle[cpu], "pb", me->piggyback, '\n');
                add_history_event(current_core_cycle[cpu], nw.instr_id, nw.phy_full_addr >> LOG2_BLOCK_SIZE, nw.phy_full_addr, TRANSLATION, "PTW_NEXT_LEVEL", "PTW", false, false, false, false, 0, 0, next_level);
            }
            else {
                // All levels done: complete the walk
                l.log("PTWComplete, vaddr", hex2str(me->vaddr),"paddr", hex2str(me->current_pte_addr), "pte_val", hex2str(next_level_base), " instr", me->instr_id, "ins", +me->packet.instruction, "current-cy", current_core_cycle[cpu], "pb", me->piggyback, '\n');
                add_history_event(current_core_cycle[cpu], me->instr_id, next_level_base >> LOG2_BLOCK_SIZE, next_level_base, TRANSLATION, "PTW_COMPLETE", "PTW", false, false, false, false, 0, 0, lvl);
                complete_page_walk(me->packet, next_level_base);
                stats.walks_completed++;
                uint64_t latency = current_core_cycle[cpu] - me->requested_cycle;
                stats.total_latency += latency;
                if (latency > stats.max_latency) stats.max_latency = latency;
            }

                // // Re-queue any piggyback walks that were waiting for the same cache block at
                // // the same level.  They were not sent to L1D directly (to avoid duplicate MSHR
                // // merges), so now that the PwC has this block's data, re-queue them as new
                // // OutstandingWalks.  operate() will re-compute their individual pte_addr values
                // // and either hit the PwC (different pte but same block — unlikely) or send their
                // // own L1D requests (which will hit the cache block and get the correct per-byte
                // // PTE via the shadow page table).
                // uint64_t primary_block = me->current_pte_addr >> LOG2_BLOCK_SIZE;
                // for (int j = 0; j < PTW_MSHR_SIZE; j++) {
                //     PTW_MSHR_Entry &pb = mshr[j];
                //     if (&pb == me) continue;
                //     if (!pb.valid || !pb.piggyback) continue;
                //     if (pb.current_level != (int)lvl) continue;
                //     if (pb.current_pte_addr >> LOG2_BLOCK_SIZE != primary_block) continue;

                //     OutstandingWalk nw;
                //     nw.virt_full_addr  = pb.vaddr;
                //     nw.current_level   = (int)lvl;
                //     nw.phy_full_addr   = pb.table_base_pa;
                //     nw.requested_cycle = pb.requested_cycle;
                //     nw.instr_id        = pb.instr_id;
                //     nw.packet          = pb.packet;
                //     for (uint32_t lv = 0; lv < PWC_TOTAL_LEVELS; lv++)
                //         nw.level_stats[lv] = pb.level_stats[lv];
                //     nw.total_page_faults = pb.total_page_faults;
                //     outstanding_walks.push_back(nw);
                //     debug.log("PiggybackRequeue, pb_vaddr", hex2str(pb.vaddr), "pb_pte", hex2str(pb.current_pte_addr), "level", lvl, "current-cy", current_core_cycle[cpu], '\n');
                //     pb.valid = false;
                // }

                // // free primary slot
                me->valid = false; // free MSHR slot
        }
    }
}

// Complete a page walk — fill result into STLB packet and return to STLB
void PTWclass::complete_page_walk(PACKET &pkt, uint64_t translated_pa) {
    pkt.instruction_pa = translated_pa;
    pkt.data_pa        = translated_pa;
    pkt.translated     = COMPLETED;
    pkt.data           = translated_pa >> LOG2_PAGE_SIZE;
    // pkt.hit_where      = hit_where_t::PTW;

    // Store PWC miss info directly in ROB entry for tracking in complete_data_fetch
    if (pkt.rob_index >= 0 && (uint32_t)pkt.rob_index < ooo_cpu[cpu].ROB.SIZE)
        ooo_cpu[cpu].ROB.entry[pkt.rob_index].pwc_miss_mem_hitwhere = pkt.pwc_miss_mem_hitwhere;

    // cerr << "[PTW_DIAG] complete_page_walk vaddr=" << hex2str(pkt.full_addr)
    //      << " translated_pa=" << hex2str(translated_pa)
    //      << " virt_addr=" << hex2str(pkt.virt_addr)
    //      << " pfn=" << hex2str(pkt.data)
    //      << " stlb_null=" << (stlb_cache == NULL ? "YES" : "NO")
    //      << " ip=" << hex2str(pkt.ip)
    //      << " cycle=" << current_core_cycle[cpu] << endl;

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

// do freelookup caches and assign if cache actually has data then went_offchip_pred=0 otherwise 1
bool PTWclass::oracle_predictor(PACKET* packet)
{
    bool in_l1d = l1d->free_lookup(packet);
    bool in_l2 = l2->free_lookup(packet);
    bool in_llc = llc->free_lookup(packet);
    bool found_on_chip = (in_l1d || in_l2 || in_llc);
    
    // if (packet->type == TRANSLATION && stats.total_walks_initiated < 20) {
    //     cout << "[ORACLE_DEBUG] pte_addr: " << hex << packet->full_addr 
    //          << " block_addr: " << (packet->address) 
    //          << " lvl: " << packet->ptw_level
    //          << " in_l1d: " << in_l1d << " in_l2: " << in_l2 << " in_llc: " << in_llc << dec << endl;
    // }
    
    packet->went_offchip_pred = !found_on_chip; // predict off-chip if not found in any on-chip cache
    return !found_on_chip; // predict off-chip if not found in any on-chip cache
}

// Print configuration
void PTWclass::print_config() {
}

// Print statistics
void PTWclass::print_stats() {

    cout << endl;
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

        cout << '\n';
    }

    cout << "Predictor," << cpu << "," << "offchip_pred, calls, sent, stalled, predict_calls, oracle_predict_calls, already_in_flight\n";
        cout << "Predictor," << cpu << "," << "offchip_pred"
             << "," << stats.pred_stats.ddrp_offchip_calls
             << "," << stats.pred_stats.ddrp_offchip_sent
             << "," << stats.pred_stats.ddrp_offchip_stalled
             << "\n";  
}
