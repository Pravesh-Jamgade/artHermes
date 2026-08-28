#include <cstdint>
#include <algorithm>
#include "cache.h"
#include "set.h"
#include "ooo_cpu.h"
#include "ptw.h"
#include "uncore.h"
#include "logging.h"
#include "buddy_allocator.h"
#include "llc_pred_perc.h"

// =====================================================================
// CACHE.CC - Cache Hierarchy Implementation
// =====================================================================
//
// This module implements the multi-level cache hierarchy for the processor,
// including TLBs (ITLB, DTLB, STLB) and data/instruction caches (L1I, L1D, L2C, LLC).
//
// Key Sections:
//   - Configuration & Initialization (print_cache_config, create_rq)
//   - Fill Operations (handle_fill)
//   - Read Operations (handle_read, check_hit)
//   - Write Operations (handle_writeback, add_wq)
//   - Prefetch Operations (handle_prefetch, prefetch_line)
//   - MSHR Management (add_mshr, check_mshr)
//   - Cache Management (fill_cache, invalidate_entry)
//   - Response Handling (return_data) - includes Pravesh page table routing
//   - Main Cycle (operate)
//
// Pravesh PTW (Page Table Walker) Integration:
//   - TRANSLATION packets are routed through PTW for caching in PWC
//   - See lines marked with "Pravesh:" for page table handling code
// =====================================================================

namespace knob
{
    // pravesh: shadowSTLB
    extern string   shadowstlb_mode;
    extern string   offchip_pred_type;
    extern uint32_t semi_perfect_cache_page_buffer_size;
    extern bool     measure_cache_acc;
    extern uint32_t measure_cache_acc_epoch;

    extern bool     l2c_dump_access_trace;
    extern bool     llc_dump_access_trace;
    extern bool     track_load_hit_dependency_in_cache;
    extern bool     l1d_perfect;
    extern bool     l2c_perfect;
    extern bool     llc_perfect;
    extern bool     llc_pseudo_perfect_enable;
    extern float    llc_pseudo_perfect_prob;
    extern bool     llc_pseudo_perfect_enable_frontal;
    extern bool     llc_pseudo_perfect_enable_dorsal;
    extern bool     l2c_pseudo_perfect_enable;
    extern float    l2c_pseudo_perfect_prob;
    extern bool     l2c_pseudo_perfect_enable_frontal;
    extern bool     l2c_pseudo_perfect_enable_dorsal;
    extern bool     enable_ptw;
    extern bool     disable_l1_translation_install;
    extern bool     knob_doa_predictor;
    extern bool     enable_ddrp;

    extern bool     ideal_stlb;
    extern bool     ideal_llc_trans;

    extern bool     offchip_pred_mark_merged_load;
    extern bool     enable_itlb_priority_rq;
    extern bool     enable_dtlb_priority_rq;
    extern bool     enable_stlb_priority_rq;
    extern bool     enable_l1i_priority_rq;
    extern bool     enable_l1d_priority_rq;
    extern bool     enable_l2c_priority_rq;
    extern bool     enable_llc_priority_rq;
    extern uint32_t itlb_priority_rq_priority_type;
    extern uint32_t dtlb_priority_rq_priority_type;
    extern uint32_t stlb_priority_rq_priority_type;
    extern uint32_t l1i_priority_rq_priority_type;
    extern uint32_t l1d_priority_rq_priority_type;
    extern uint32_t l2c_priority_rq_priority_type;
    extern uint32_t llc_priority_rq_priority_type;
    extern uint32_t stlb_set;
    extern uint32_t stlb_way;
    extern uint32_t stlb_latency;
    extern uint32_t itlb_latency;
    extern uint32_t dtlb_latency;
    extern uint32_t l1i_latency;
    extern uint32_t l1d_latency;
    extern uint32_t l2c_latency;
    extern uint32_t llc_latency;
    extern string   max_lru_before_eviction_block_type;
    extern uint32_t translation_extra_latency;
}

uint64_t l2pf_access = 0;

void print_cache_config()
{
    cout << "itlb_set " << ITLB_SET << endl
        << "itlb_way " << ITLB_WAY << endl
        << "itlb_rq_size " << ITLB_RQ_SIZE << endl
        << "itlb_wq_size " << ITLB_WQ_SIZE << endl
        << "itlb_pq_size " << ITLB_PQ_SIZE << endl
        << "itlb_mshr_size " << ITLB_MSHR_SIZE << endl
        << "itlb_latency " << knob::itlb_latency << endl
        << "itlb_priority_rq " << +knob::enable_itlb_priority_rq << endl
        << "itlb_priority_rq_type " << priority_name_string[knob::itlb_priority_rq_priority_type] << endl
        << endl
        << "dtlb_set " << DTLB_SET << endl
        << "dtlb_way " << DTLB_WAY << endl
        << "dtlb_rq_size " << DTLB_RQ_SIZE << endl
        << "dtlb_wq_size " << DTLB_WQ_SIZE << endl
        << "dtlb_pq_size " << DTLB_PQ_SIZE << endl
        << "dtlb_mshr_size " << DTLB_MSHR_SIZE << endl
        << "dtlb_latency " << knob::dtlb_latency << endl
        << "dtlb_priority_rq " << +knob::enable_dtlb_priority_rq << endl
        << "dtlb_priority_rq_type " << priority_name_string[knob::dtlb_priority_rq_priority_type] << endl
        << endl
        << "stlb_set " << knob::stlb_set << endl
        << "stlb_way " << knob::stlb_way << endl
        << "stlb_rq_size " << STLB_RQ_SIZE << endl
        << "stlb_wq_size " << STLB_WQ_SIZE << endl
        << "stlb_pq_size " << STLB_PQ_SIZE << endl
        << "stlb_mshr_size " << STLB_MSHR_SIZE << endl
        << "stlb_latency " << knob::stlb_latency << endl
        << "stlb_priority_rq " << +knob::enable_stlb_priority_rq << endl
        << "stlb_priority_rq_type " << priority_name_string[knob::stlb_priority_rq_priority_type] << endl
        << endl
        << "l1i_size " << (L1I_SET*L1I_WAY*BLOCK_SIZE)/1024 << endl
        << "l1i_set " << L1I_SET << endl
        << "l1i_way " << L1I_WAY << endl
        << "l1i_rq_size " << L1I_RQ_SIZE << endl
        << "l1i_wq_size " << L1I_WQ_SIZE << endl
        << "l1i_pq_size " << L1I_PQ_SIZE << endl
        << "l1i_mshr_size " << L1I_MSHR_SIZE << endl
        << "l1i_latency " << knob::l1i_latency << endl
        << "l1i_priority_rq " << +knob::enable_l1i_priority_rq << endl
        << "l1i_priority_rq_type " << priority_name_string[knob::l1i_priority_rq_priority_type] << endl
        << endl
        << "l1d_size " << (L1D_SET*L1D_WAY*BLOCK_SIZE)/1024 << endl
        << "l1d_set " << L1D_SET << endl
        << "l1d_way " << L1D_WAY << endl
        << "l1d_rq_size " << L1D_RQ_SIZE << endl
        << "l1d_wq_size " << L1D_WQ_SIZE << endl
        << "l1d_pq_size " << L1D_PQ_SIZE << endl
        << "l1d_mshr_size " << L1D_MSHR_SIZE << endl
        << "l1d_latency " << knob::l1d_latency << endl
        << "l1d_priority_rq " << +knob::enable_l1d_priority_rq << endl
        << "l1d_priority_rq_type " << priority_name_string[knob::l1d_priority_rq_priority_type] << endl
        << endl
        << "l2c_size " << (L2C_SET*L2C_WAY*BLOCK_SIZE)/1024 << endl
        << "l2c_set " << L2C_SET << endl
        << "l2c_way " << L2C_WAY << endl
        << "l2c_rq_size " << L2C_RQ_SIZE << endl
        << "l2c_wq_size " << L2C_WQ_SIZE << endl
        << "l2c_pq_size " << L2C_PQ_SIZE << endl
        << "l2c_mshr_size " << L2C_MSHR_SIZE << endl
        << "l2c_latency " << knob::l2c_latency << endl
        << "l2c_priority_rq " << +knob::enable_l2c_priority_rq << endl
        << "l2c_priority_rq_type " << priority_name_string[knob::l2c_priority_rq_priority_type] << endl
        << endl
        << "llc_size " << (LLC_SET*LLC_WAY*BLOCK_SIZE)/1024 << endl
        << "llc_set " << LLC_SET << endl
        << "llc_way " << LLC_WAY << endl
        << "llc_rq_size " << LLC_RQ_SIZE << endl
        << "llc_wq_size " << LLC_WQ_SIZE << endl
        << "llc_pq_size " << LLC_PQ_SIZE << endl
        << "llc_mshr_size " << LLC_MSHR_SIZE << endl
        << "llc_latency " << knob::llc_latency << endl
        << "llc_priority_rq " << +knob::enable_llc_priority_rq << endl
        << "llc_priority_rq_type " << priority_name_string[knob::llc_priority_rq_priority_type] << endl
        << endl;
}

// =====================================================================
// CONFIGURATION & INITIALIZATION
// =====================================================================

void CACHE::create_rq()
{
    // create RQ appropriately
    bool priority_rq = false;
    uint32_t priority_type = 0;
    if (cache_type == IS_ITLB && knob::enable_itlb_priority_rq)        {priority_rq = true; priority_type = knob::itlb_priority_rq_priority_type;}
    else if (cache_type == IS_DTLB && knob::enable_dtlb_priority_rq)   {priority_rq = true; priority_type = knob::dtlb_priority_rq_priority_type;}
    else if (cache_type == IS_STLB && knob::enable_stlb_priority_rq)   {priority_rq = true; priority_type = knob::stlb_priority_rq_priority_type;}
    else if (cache_type == IS_L1I && knob::enable_l1i_priority_rq)     {priority_rq = true; priority_type = knob::l1i_priority_rq_priority_type;}
    else if (cache_type == IS_L1D && knob::enable_l1d_priority_rq)     {priority_rq = true; priority_type = knob::l1d_priority_rq_priority_type;}
    else if (cache_type == IS_L2C && knob::enable_l2c_priority_rq)     {priority_rq = true; priority_type = knob::l2c_priority_rq_priority_type;}
    else if (cache_type == IS_LLC && knob::enable_llc_priority_rq)     {priority_rq = true; priority_type = knob::llc_priority_rq_priority_type;}

    if(priority_rq)
    {
        if(priority_type == 2 || priority_type == 3)
        {
            assert(knob::offchip_pred_type != "none");
        }
        cout << "Adding priority-RQ type " << priority_type << " in " << NAME << endl;
        RQ = new PACKET_QUEUE_PRIORITY((priority_type_t)priority_type);
        RQ->init(RQ_SIZE);
        RQ->is_RQ = 1;
        RQ->NAME = NAME + "_RQ";
        RQ->deduce_module_queue_types();
    }
    else
    {
        cout << "Adding basic-RQ in " << NAME << endl;
        RQ = new PACKET_QUEUE();
        RQ->init(RQ_SIZE);
        RQ->is_RQ = 1;
        RQ->NAME = NAME + "_RQ";
        RQ->deduce_module_queue_types();
    }
}

// =====================================================================
// CACHE FILL OPERATIONS - Complete cache fill from next level
// =====================================================================

void CACHE::handle_fill()
{
    // handle fill
    uint32_t fill_cpu = (MSHR.next_fill_index == MSHR_SIZE) ? NUM_CPUS : MSHR.entry[MSHR.next_fill_index].cpu;
    if (fill_cpu == NUM_CPUS)
    {
        return;
    }

    if (MSHR.next_fill_cycle <= current_core_cycle[fill_cpu]) 
    {

#ifdef SANITY_CHECK
        if (MSHR.next_fill_index >= MSHR.SIZE)
        {
            cerr << "[" << NAME << "] MSHR index out of bounds: " << MSHR.next_fill_index << " >= " << MSHR.SIZE << endl;
            assert(0 && "MSHR index sanity check failed");
        }
#endif

        uint32_t mshr_index = MSHR.next_fill_index;

        // If knob::disable_l1_translation_install is true, do not insert TRANSLATION blocks into L1 caches (L1D or L1I)
        if (knob::disable_l1_translation_install && (cache_type == IS_L1D || cache_type == IS_L1I) && (MSHR.entry[mshr_index].from_ptw && MSHR.entry[mshr_index].type == TRANSLATION))
        {
            // Trigger history event
            add_history_event(current_core_cycle[fill_cpu], MSHR.entry[mshr_index].instr_id, MSHR.entry[mshr_index].virt_addr, MSHR.entry[mshr_index].address, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type, "CACHE_FILL_BYPASS_TRANS", NAME.c_str(), false, false, false, false, 0, MSHR.entry[mshr_index].hit_where);

            // // send response to upper level cache (usually translation packets are not forwarded to core data cache,
            // // but we must notify the Page Table Walker if it originated from PTW)
            if (cache_type == IS_L1D) // since ptw for all d or i goes through d caches only
            {
                if (ooo_cpu[MSHR.entry[mshr_index].cpu].page_table_walker != NULL) {
                    ooo_cpu[MSHR.entry[mshr_index].cpu].page_table_walker->handle_memory_response(&MSHR.entry[mshr_index]);
                }
            }

            // update miss latency stats
            if (warmup_complete[fill_cpu] && (MSHR.entry[mshr_index].cycle_enqueued != 0))
            {
                uint64_t current_miss_latency = (current_core_cycle[fill_cpu] - MSHR.entry[mshr_index].cycle_enqueued);
                total_miss_latency += current_miss_latency;
            }

            uint64_t deque_cycle = current_core_cycle[fill_cpu];
            if (deque_cycle >= MSHR.entry[mshr_index].enque_cycle[cache_type][IS_RQ]) {
                service_time_hist.update(deque_cycle - MSHR.entry[mshr_index].enque_cycle[cache_type][IS_RQ]);
            }


            uint32_t set = get_set(MSHR.entry[mshr_index].address);
            for (uint32_t way = 0; way < NUM_WAY; way++) {
                if (block[set][way].valid && (block[set][way].tag == MSHR.entry[mshr_index].address)) {
                    cout << " why addr, " << hex2str(MSHR.entry[mshr_index].address) << ", full_addr, " << hex2str(MSHR.entry[mshr_index].full_addr) << ", instr, " << MSHR.entry[mshr_index].instr_id << '\n';
                    exit(0);
                }
            }

            MSHR.remove_queue(&MSHR.entry[mshr_index]);
            MSHR.num_returned--;

            update_fill_cycle();

            return;
        }

        if((cache_type == IS_L1D || cache_type == IS_L1I) && MSHR.entry[mshr_index].from_ptw && MSHR.entry[mshr_index].type == TRANSLATION)
        {
            cout << "Write, addr, " << hex2str(MSHR.entry[mshr_index].address) << ", instrid, " << MSHR.entry[mshr_index].instr_id << ", $, " << NAME << "ptw?, " << +MSHR.entry[mshr_index].from_ptw << ", type, " << +MSHR.entry[mshr_index].type << '\n'; 
            cout << "why?";exit(0);
        }

        // find victim
        uint32_t set = get_set(MSHR.entry[mshr_index].address), way;
        if (cache_type == IS_LLC) 
        {
            way = llc_find_victim(fill_cpu, MSHR.entry[mshr_index].instr_id, set, block[set], MSHR.entry[mshr_index].ip, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type);
        }
        else
        {
            way = find_victim(fill_cpu, MSHR.entry[mshr_index].instr_id, set, block[set], MSHR.entry[mshr_index].ip, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type);
        }

#ifdef LLC_BYPASS
        if ((cache_type == IS_LLC) && (way == LLC_WAY)) // this is a bypass that does not fill the LLC
        { 
            // update replacement policy
            if (cache_type == IS_LLC) 
            {
                llc_update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, 0, MSHR.entry[mshr_index].type, 0);
            }
            else
            {
                update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, 0, MSHR.entry[mshr_index].type, 0);
            }

            // COLLECT STATS
            sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
            sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;
            add_history_event(current_core_cycle[fill_cpu], MSHR.entry[mshr_index].instr_id, MSHR.entry[mshr_index].virt_addr, MSHR.entry[mshr_index].address, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type, "CACHE_FILL_BYPASS", NAME.c_str(), false, false, false, false, 0, MSHR.entry[mshr_index].hit_where);

            // check fill level
            if (MSHR.entry[mshr_index].fill_level < fill_level) 
            {
                if(fill_level == FILL_L2)
		        {
                    if(MSHR.entry[mshr_index].fill_l1i)
                    {
                        upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                    }
                    if(MSHR.entry[mshr_index].fill_l1d)
                    {
                        upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                    }
		        }
                else
                {
                    if (MSHR.entry[mshr_index].instruction)
                    {
                        upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                    }
                    if (MSHR.entry[mshr_index].is_data)
                    {
                        upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                    }
                }
            }

            if(warmup_complete[fill_cpu] && (MSHR.entry[mshr_index].cycle_enqueued != 0))
            {
                uint64_t current_miss_latency = (current_core_cycle[fill_cpu] - MSHR.entry[mshr_index].cycle_enqueued);
                total_miss_latency += current_miss_latency;
            }

            MSHR.remove_queue(&MSHR.entry[mshr_index]);
            MSHR.num_returned--;

            update_fill_cycle();

            return; // return here, no need to process further in this function
        }
#endif

        uint8_t  do_fill = 1;

        // is this dirty?
        if (block[set][way].dirty)
        {
            // check if the lower level WQ has enough room to keep this writeback request
            if (lower_level) 
            {
                if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address)) 
                {
                    // lower level WQ is full, cannot replace this victim
                    do_fill = 0;
                    lower_level->increment_WQ_FULL(block[set][way].address);
                    STALL[MSHR.entry[mshr_index].type]++;

                    DP ( if (warmup_complete[fill_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                    cout << " lower level wq is full!" << " fill_addr: " << hex << MSHR.entry[mshr_index].address;
                    cout << " victim_addr: " << block[set][way].tag << dec << endl; });
                }
                else 
                {
                    PACKET writeback_packet;

                    writeback_packet.fill_level = fill_level << 1;
                    writeback_packet.cpu = fill_cpu;
                    writeback_packet.address = block[set][way].address;
                    writeback_packet.full_addr = block[set][way].full_addr;
                    writeback_packet.data = block[set][way].data;
                    writeback_packet.instr_id = MSHR.entry[mshr_index].instr_id;
                    writeback_packet.ip = 0; // writeback does not have ip
                    writeback_packet.type = WRITEBACK;
                    writeback_packet.asid[0] = block[set][way].asid[0];
                    writeback_packet.asid[1] = block[set][way].asid[1];
                    writeback_packet.event_cycle = current_core_cycle[fill_cpu];

                    lower_level->add_wq(&writeback_packet);
                }
            }
#ifdef SANITY_CHECK
            else 
            {
                // sanity check
                if (cache_type != IS_STLB) {
                    cerr << "[" << NAME << "] Cache has no lower level but is not STLB. Packet addr: 0x" << hex << block[set][way].address << dec << endl;
                    assert(0 && "Non-STLB cache missing lower level");
                }
            }
#endif
        }

        if (do_fill)
        {
            // update prefetcher
            if (cache_type == IS_L1I)
            {
                l1i_prefetcher_cache_fill(fill_cpu, ((MSHR.entry[mshr_index].ip)>>LOG2_BLOCK_SIZE)<<LOG2_BLOCK_SIZE, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0, ((block[set][way].ip)>>LOG2_BLOCK_SIZE)<<LOG2_BLOCK_SIZE);
            }
            if (cache_type == IS_L1D)
            {
                l1d_prefetcher_cache_fill(MSHR.entry[mshr_index].full_addr, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0, block[set][way].address<<LOG2_BLOCK_SIZE, MSHR.entry[mshr_index].pf_metadata);
            }
            if (cache_type == IS_L2C)
            {
	            MSHR.entry[mshr_index].pf_metadata = l2c_prefetcher_cache_fill(MSHR.entry[mshr_index].address<<LOG2_BLOCK_SIZE, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0, block[set][way].address<<LOG2_BLOCK_SIZE, MSHR.entry[mshr_index].pf_metadata);
            }
            if (cache_type == IS_LLC)
	        {
		        cpu = fill_cpu;
		        MSHR.entry[mshr_index].pf_metadata = llc_prefetcher_cache_fill(MSHR.entry[mshr_index].address<<LOG2_BLOCK_SIZE, set, way, (MSHR.entry[mshr_index].type == PREFETCH) ? 1 : 0, block[set][way].address<<LOG2_BLOCK_SIZE, MSHR.entry[mshr_index].pf_metadata);
		        cpu = 0;
	        }
              
            // update replacement policy
            if (cache_type == IS_LLC) 
            {
                llc_update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, block[set][way].full_addr, MSHR.entry[mshr_index].type, 0);
            }
            else
            {
                update_replacement_state(fill_cpu, set, way, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip, block[set][way].full_addr, MSHR.entry[mshr_index].type, 0);
            }

            // update off-chip predictor for LLC evictions
            if (cache_type == IS_LLC)
            {
                ooo_cpu[fill_cpu].offchip_predictor_track_llc_eviction(set, way, block[set][way].full_addr);
            }

            // // @RBERA: moved this code to handle_read
            // // @RBERA: if this is a load fill in LLC, then monitor the position of the load in ROB
            // if(cache_type == IS_LLC && MSHR.entry[mshr_index].type == LOAD)
            // {
                
            //     if(MSHR.entry[mshr_index].rob_position < 0 && MSHR.entry[mshr_index].rob_position >= ROB_SIZE)
            //     {
            //         cout << "invalid ROB position: index: " << mshr_index << " pos: " << MSHR.entry[mshr_index].rob_position << endl;
            //         assert(0); 
            //     }
            //     // missing_load_rob_pos_hist[MSHR.entry[mshr_index].rob_position]++;
            // }

            // COLLECT STATS
            sim_miss[fill_cpu][MSHR.entry[mshr_index].type]++;
            sim_access[fill_cpu][MSHR.entry[mshr_index].type]++;

            // RBERA-TODO: Dump cache access trace
            if(knob::l2c_dump_access_trace && cache_type == IS_L2C && warmup_complete[fill_cpu])
            {
                tracer.record_trace(MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type, false);
            }
            if(knob::llc_dump_access_trace && cache_type == IS_LLC && warmup_complete[fill_cpu])
            {
                tracer.record_trace(MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type, false);
            }

            fill_cache(set, way, &MSHR.entry[mshr_index]);
            add_history_event(current_core_cycle[fill_cpu], MSHR.entry[mshr_index].instr_id, MSHR.entry[mshr_index].virt_addr, MSHR.entry[mshr_index].address, MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].type, "CACHE_FILL", NAME.c_str(), false, false, false, false, 0, MSHR.entry[mshr_index].hit_where);

            // Deadblock pred: store prediction in block on fill, reset usage, train
            if (cache_type == IS_LLC && MSHR.entry[mshr_index].type == LOAD 
                && knob::knob_doa_predictor && llc_pred_perc != NULL)
            {
                bool pred = llc_pred_perc->predict(MSHR.entry[mshr_index].full_addr, MSHR.entry[mshr_index].ip);
                block[set][way].doa_pred_bit = pred; 
                block[set][way].usage = 0; // reset usage on new install
            }

            // RFO marks cache line dirty
            if (cache_type == IS_L1D)
            {
                if (MSHR.entry[mshr_index].type == RFO)
                {
                    block[set][way].dirty = 1;
                }
            }

            // send response to upper level cache
            if (MSHR.entry[mshr_index].fill_level < fill_level) 
            {
	            if(fill_level == FILL_L2)
                {
                    if(MSHR.entry[mshr_index].fill_l1i)
                    {
                        upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                    }
                    if(MSHR.entry[mshr_index].fill_l1d)
                    {
                        upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                    }
                }
	            else
                {
                    if (MSHR.entry[mshr_index].instruction)
                    {
                        upper_level_icache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                    }
                    if (MSHR.entry[mshr_index].is_data)
                    {
                                upper_level_dcache[fill_cpu]->return_data(&MSHR.entry[mshr_index]);
                    }
                }
            }

            // update processed packets
            if (cache_type == IS_ITLB) 
            { 
                MSHR.entry[mshr_index].instruction_pa = block[set][way].data;
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                {
                    PROCESSED.add_queue(&MSHR.entry[mshr_index], current_core_cycle[fill_cpu]);
                }
            }
            else if (cache_type == IS_DTLB) 
            {
                MSHR.entry[mshr_index].data_pa = block[set][way].data;
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                {
                    PROCESSED.add_queue(&MSHR.entry[mshr_index], current_core_cycle[fill_cpu]);
                }
            }
            else if (cache_type == IS_L1I)
            {
                if (PROCESSED.occupancy < PROCESSED.SIZE)
                {
                    PROCESSED.add_queue(&MSHR.entry[mshr_index], current_core_cycle[fill_cpu]);
                }
            }
            //else if (cache_type == IS_L1D) {
            // Pravesh: Page table translation handling - allow TRANSLATION packets from L1D
            else if ((cache_type == IS_L1D) && (MSHR.entry[mshr_index].type != PREFETCH)) 
            {
                // Allow TRANSLATION packets from L1D to proceed through PWC -> STLB -> DTLB
                if (MSHR.entry[mshr_index].type == TRANSLATION)
                {
                    // if this packet originated from the PTW module, notify it of the response
                    if (ooo_cpu[MSHR.entry[mshr_index].cpu].page_table_walker != NULL) {
                        ooo_cpu[MSHR.entry[mshr_index].cpu].page_table_walker->handle_memory_response(&MSHR.entry[mshr_index]);
                    }
                }
                else if (PROCESSED.occupancy < PROCESSED.SIZE)
                {
                    PROCESSED.add_queue(&MSHR.entry[mshr_index], current_core_cycle[fill_cpu]);
                }
            }

            if(warmup_complete[fill_cpu] && (MSHR.entry[mshr_index].cycle_enqueued != 0))
            {
                uint64_t current_miss_latency = (current_core_cycle[fill_cpu] - MSHR.entry[mshr_index].cycle_enqueued);
                /*
                if(cache_type == IS_L1D)
                {
                    cout << current_core_cycle[fill_cpu] << " - " << MSHR.entry[mshr_index].cycle_enqueued << " = " << current_miss_latency << " MSHR index: " << mshr_index << endl;
                }
                */
                total_miss_latency += current_miss_latency;
            }
      
            uint64_t deque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[fill_cpu];
            if (deque_cycle >= MSHR.entry[mshr_index].enque_cycle[cache_type][IS_RQ]) {
                service_time_hist.update(deque_cycle - MSHR.entry[mshr_index].enque_cycle[cache_type][IS_RQ]);
            }
            MSHR.remove_queue(&MSHR.entry[mshr_index]);
            MSHR.num_returned--;

            update_fill_cycle();
        }
    }
}

// =====================================================================
// WRITE-BACK OPERATIONS - Flush modified cache lines to next level
// =====================================================================

void CACHE::handle_writeback()
{
    // handle write
    uint32_t writeback_cpu = WQ.entry[WQ.head].cpu;
    if (writeback_cpu == NUM_CPUS)
    {
        return;
    }

    uint64_t current_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[writeback_cpu];

    if (WQ.entry[WQ.head].started_latency == 0) {
        WQ.entry[WQ.head].event_cycle = current_cycle + LATENCY;
        WQ.entry[WQ.head].started_latency = 1;
    }

    // handle the oldest entry
    if ((WQ.entry[WQ.head].event_cycle <= current_cycle) && (WQ.occupancy > 0)) 
    {
        int index = WQ.head;

        // access cache
        uint32_t set = get_set(WQ.entry[index].address);
        int way = check_hit(&WQ.entry[index]);
        
        if (way >= 0) // writeback hit (or RFO hit for L1D)
        {
            WQ.entry[index].hit_where = assign_hit_where(cache_type, 0); // writeback hit
            
            // Update footprint on hit
            if (block[set][way].footprint.track_footprint) {
                uint32_t word_num = (WQ.entry[index].full_addr >> 3) & 0x7;
                uint8_t word_bit = (1 << word_num);
                if ((block[set][way].footprint.footprint & word_bit) == 0) {
                    block[set][way].footprint.footprint |= word_bit;
                    if (block[set][way].lru > block[set][way].footprint.max_lru_footprint_change) {
                        block[set][way].footprint.max_lru_footprint_change = block[set][way].lru;
                    }
                    block[set][way].footprint.footprint_changed = true;
                } else {
                    block[set][way].footprint.entry_reuse[word_num]++;
                }
            }

            if (cache_type == IS_LLC) 
            {
                llc_update_replacement_state(writeback_cpu, set, way, block[set][way].full_addr, WQ.entry[index].ip, 0, WQ.entry[index].type, 1);
            }
            else
            {
                update_replacement_state(writeback_cpu, set, way, block[set][way].full_addr, WQ.entry[index].ip, 0, WQ.entry[index].type, 1);
            }

            // COLLECT STATS
            sim_hit[writeback_cpu][WQ.entry[index].type]++;
            sim_access[writeback_cpu][WQ.entry[index].type]++;

            // @RBERA: populate reuse metadata
            block[set][way].reuse[WQ.entry[index].type]++;

            // RBERA-TODO: Dump cache access trace
            if(knob::l2c_dump_access_trace && cache_type == IS_L2C && warmup_complete[writeback_cpu])
            {
                tracer.record_trace(WQ.entry[index].full_addr, WQ.entry[index].type, true);
            }
            if(knob::llc_dump_access_trace && cache_type == IS_LLC && warmup_complete[writeback_cpu])
            {
                tracer.record_trace(WQ.entry[index].full_addr, WQ.entry[index].type, true);
            }

            // mark dirty
            block[set][way].dirty = 1;

            if (cache_type == IS_ITLB)
            {
                WQ.entry[index].instruction_pa = block[set][way].data;
            }
            else if (cache_type == IS_DTLB)
            {
                WQ.entry[index].data_pa = block[set][way].data;
            }
            else if (cache_type == IS_STLB)
            {
                WQ.entry[index].data = block[set][way].data;
            }

            // send the response to upper level
            if (WQ.entry[index].fill_level < fill_level) 
            {
                if(fill_level == FILL_L2)
                {
                    if(WQ.entry[index].fill_l1i)
                    {
                        upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
                    }
                    if(WQ.entry[index].fill_l1d)
                    {
                        upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
                    }
                }
                else
                {
                    if (WQ.entry[index].instruction)
                    {
                            upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
                    }
                    if (WQ.entry[index].is_data)
                    {
                            upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
                    }
                }
            }

            // old code
            HIT[WQ.entry[index].type]++;
            ACCESS[WQ.entry[index].type]++;

            // remove this entry from WQ
            uint64_t deque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[writeback_cpu];
            WQ.remove_queue(&WQ.entry[index], deque_cycle);
        }
        else // writeback miss (or RFO miss for L1D)
        { 
            DP ( if (warmup_complete[writeback_cpu]) {
            cout << "[" << NAME << "] " << __func__ << " type: " << +WQ.entry[index].type << " miss";
            cout << " instr_id: " << WQ.entry[index].instr_id << " address: " << hex << WQ.entry[index].address;
            cout << " full_addr: " << WQ.entry[index].full_addr << dec;
            cout << " cycle: " << WQ.entry[index].event_cycle << endl; });

            if (cache_type == IS_L1D) // RFO miss
            { 
                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&WQ.entry[index]);

		        if(mshr_index == -2)
		        {
		            // this is a data/instruction collision in the MSHR, so we have to wait before we can allocate this miss
		            miss_handled = 0;
		        }
                else if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) // this is a new miss
                { 
		            if(cache_type == IS_LLC)
		            {
		                // check to make sure the DRAM RQ has room for this LLC RFO miss
		                if (lower_level->get_occupancy(1, WQ.entry[index].address) == lower_level->get_size(1, WQ.entry[index].address))
                        {
                            miss_handled = 0;
                        }
                        else
                        {
                            add_mshr(&WQ.entry[index]);
                            lower_level->add_rq(&WQ.entry[index]);
                        }
		            }
                    else
                    {
                        // check to make sure the DRAM RQ has room for this LLC RFO miss
		                if (lower_level && lower_level->get_occupancy(1, WQ.entry[index].address) == lower_level->get_size(1, WQ.entry[index].address))
                        {
                            miss_handled = 0;
                        }
                        else
                        {
                            // add it to mshr (RFO miss)
                            add_mshr(&WQ.entry[index]);
                    
                            // add it to the next level's read queue
                            //if (lower_level) // L1D always has a lower level cache
                            lower_level->add_rq(&WQ.entry[index]);
                        }
                    }
                }
                else 
                {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) // not enough MSHR resource
                    { 
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[WQ.entry[index].type]++;
                    }
                    else if (mshr_index != -1) // already in-flight miss
                    {
                        WQ.entry[index].hit_where = assign_hit_where(cache_type, 3); // writeback hit in MSHR
                        
                        // update fill_level
                        if (WQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
                        {
                            MSHR.entry[mshr_index].fill_level = WQ.entry[index].fill_level;
                        }
                        if((WQ.entry[index].fill_l1i) && (MSHR.entry[mshr_index].fill_l1i != 1))
                        {
                            MSHR.entry[mshr_index].fill_l1i = 1;
                        }
                        if((WQ.entry[index].fill_l1d) && (MSHR.entry[mshr_index].fill_l1d != 1))
                        {
                            MSHR.entry[mshr_index].fill_l1d = 1;
                        }

                        // update request
                        if (MSHR.entry[mshr_index].type == PREFETCH) 
                        {
                            uint8_t  prior_returned = MSHR.entry[mshr_index].returned;
                            uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
			                MSHR.entry[mshr_index] = WQ.entry[index];

                            // in case request is already returned, we should keep event_cycle and retunred variables
                            MSHR.entry[mshr_index].returned = prior_returned;
                            MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
                        }

                        MSHR_MERGED[WQ.entry[index].type]++;

                        DP ( if (warmup_complete[writeback_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << WQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << WQ.entry[index].address;
                        cout << " full_addr: " << WQ.entry[index].full_addr << dec;
                        cout << " cycle: " << WQ.entry[index].event_cycle << endl; });
                    }
                    else // WE SHOULD NOT REACH HERE
                    {
                        cerr << "[" << NAME << "] WQ MSHR errors: addr=0x" << hex << WQ.entry[index].address
                             << " full_addr=0x" << WQ.entry[index].full_addr << dec
                             << " instr_id=" << WQ.entry[index].instr_id << " type=" << +WQ.entry[index].type << endl;
                        assert(0 && "WQ MSHR processing error");
                    }
                }

                if (miss_handled) 
                {
                    MISS[WQ.entry[index].type]++;
                    ACCESS[WQ.entry[index].type]++;

                    // remove this entry from WQ
                    uint64_t deque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[writeback_cpu];
                    WQ.remove_queue(&WQ.entry[index], deque_cycle);
                }

            }
            else 
            {
                // find victim
                uint32_t set = get_set(WQ.entry[index].address), way;
                if (cache_type == IS_LLC) 
                {
                    way = llc_find_victim(writeback_cpu, WQ.entry[index].instr_id, set, block[set], WQ.entry[index].ip, WQ.entry[index].full_addr, WQ.entry[index].type);
                }
                else
                {
                    way = find_victim(writeback_cpu, WQ.entry[index].instr_id, set, block[set], WQ.entry[index].ip, WQ.entry[index].full_addr, WQ.entry[index].type);
                }

#ifdef LLC_BYPASS
                if ((cache_type == IS_LLC) && (way == LLC_WAY)) 
                {
                    cerr << "[" << NAME << "_ERROR] LLC bypassing for writebacks not allowed: way=" << way;
                    assert(0 && "LLC bypass attempted for writeback");
                }
#endif

                uint8_t  do_fill = 1;

                // is this dirty?
                if (block[set][way].dirty) 
                {
                    // check if the lower level WQ has enough room to keep this writeback request
                    if (lower_level) 
                    { 
                        if (lower_level->get_occupancy(2, block[set][way].address) == lower_level->get_size(2, block[set][way].address)) 
                        {
                            // lower level WQ is full, cannot replace this victim
                            do_fill = 0;
                            lower_level->increment_WQ_FULL(block[set][way].address);
                            STALL[WQ.entry[index].type]++;

                            DP ( if (warmup_complete[writeback_cpu]) {
                            cout << "[" << NAME << "] " << __func__ << "do_fill: " << +do_fill;
                            cout << " lower level wq is full!" << " fill_addr: " << hex << WQ.entry[index].address;
                            cout << " victim_addr: " << block[set][way].tag << dec << endl; });
                        }
                        else 
                        { 
                            PACKET writeback_packet;

                            writeback_packet.fill_level = fill_level << 1;
                            writeback_packet.cpu = writeback_cpu;
                            writeback_packet.address = block[set][way].address;
                            writeback_packet.full_addr = block[set][way].full_addr;
                            writeback_packet.data = block[set][way].data;
                            writeback_packet.instr_id = WQ.entry[index].instr_id;
                            writeback_packet.ip = 0;
                            writeback_packet.type = WRITEBACK;
                            writeback_packet.asid[0] = block[set][way].asid[0];
                            writeback_packet.asid[1] = block[set][way].asid[1];
                            writeback_packet.event_cycle = current_core_cycle[writeback_cpu];

                            lower_level->add_wq(&writeback_packet);
                        }
                    }
#ifdef SANITY_CHECK
                    else 
                    {
                        // sanity check
                        if (cache_type != IS_STLB) {
                            cerr << "[" << NAME << "_ERROR] Invalid cache type for this operation: cache_type=" << cache_type;
                            assert(0 && "Expected STLB cache type");
                        }
                    }
#endif
                }

                if (do_fill) 
                {
                    // update prefetcher
                    // RBERA: why is the prefetcher_fill called for writeback fills?
                    // RBERA: this sounds incorrect!
		            if (cache_type == IS_L1I)
                    {
                            l1i_prefetcher_cache_fill(writeback_cpu, ((WQ.entry[index].ip)>>LOG2_BLOCK_SIZE)<<LOG2_BLOCK_SIZE, set, way, 0, ((block[set][way].ip)>>LOG2_BLOCK_SIZE)<<LOG2_BLOCK_SIZE);
                    }
                    if (cache_type == IS_L1D)
                    {
		                  l1d_prefetcher_cache_fill(WQ.entry[index].full_addr, set, way, 0, block[set][way].address<<LOG2_BLOCK_SIZE, WQ.entry[index].pf_metadata);
                    }
                    else if (cache_type == IS_L2C)
                    {
		                    WQ.entry[index].pf_metadata = l2c_prefetcher_cache_fill(WQ.entry[index].address<<LOG2_BLOCK_SIZE, set, way, 0, block[set][way].address<<LOG2_BLOCK_SIZE, WQ.entry[index].pf_metadata);
                    }
                    if (cache_type == IS_LLC)
		            {
                        cpu = writeback_cpu;
                        WQ.entry[index].pf_metadata =llc_prefetcher_cache_fill(WQ.entry[index].address<<LOG2_BLOCK_SIZE, set, way, 0, block[set][way].address<<LOG2_BLOCK_SIZE, WQ.entry[index].pf_metadata);
                        cpu = 0;
                    }

                    // update replacement policy
                    if (cache_type == IS_LLC) 
                    {
                        llc_update_replacement_state(writeback_cpu, set, way, WQ.entry[index].full_addr, WQ.entry[index].ip, block[set][way].full_addr, WQ.entry[index].type, 0);
                    }
                    else
                    {
                        update_replacement_state(writeback_cpu, set, way, WQ.entry[index].full_addr, WQ.entry[index].ip, block[set][way].full_addr, WQ.entry[index].type, 0);
                    }

                    // COLLECT STATS
                    sim_miss[writeback_cpu][WQ.entry[index].type]++;
                    sim_access[writeback_cpu][WQ.entry[index].type]++;

                    // RBERA-TODO: Dump cache access trace
                    if(knob::l2c_dump_access_trace && cache_type == IS_L2C && warmup_complete[writeback_cpu])
                    {
                        tracer.record_trace(WQ.entry[index].full_addr, WQ.entry[index].type, false);
                    }
                    if(knob::llc_dump_access_trace && cache_type == IS_LLC && warmup_complete[writeback_cpu])
                    {
                        tracer.record_trace(WQ.entry[index].full_addr, WQ.entry[index].type, false);
                    }

                    fill_cache(set, way, &WQ.entry[index]);

                    // mark dirty
                    block[set][way].dirty = 1; 

                    // check fill level
                    if (WQ.entry[index].fill_level < fill_level) 
                    {
		                if(fill_level == FILL_L2)
                        {
                            if(WQ.entry[index].fill_l1i)
                            {
                                upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
                            }
                            if(WQ.entry[index].fill_l1d)
                            {
                                upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
                            }
                        }
                        else
                        {
                            if (WQ.entry[index].instruction)
                            {
                                upper_level_icache[writeback_cpu]->return_data(&WQ.entry[index]);
                            }
                            if (WQ.entry[index].is_data)
                            {
                                upper_level_dcache[writeback_cpu]->return_data(&WQ.entry[index]);
                            }
                        }
                    }

                    MISS[WQ.entry[index].type]++;
                    ACCESS[WQ.entry[index].type]++;

                    // remove this entry from WQ
                    uint64_t deque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[writeback_cpu];
                    WQ.remove_queue(&WQ.entry[index], deque_cycle);
                }
            }
        }
    }
}

// =====================================================================
// READ OPERATIONS - Process read requests and cache hits/misses
// =====================================================================

void CACHE::handle_read()
{
    // handle read
    for (uint32_t i=0; i<MAX_READ; i++) 
    {
        if(RQ->is_empty())
        {
            return;
        }
        
        uint32_t read_cpu = RQ->peek().cpu;
        uint64_t current_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[read_cpu];

        if (RQ->peek().started_latency == 0) {
            PACKET& head_entry = RQ->peek();
            head_entry.event_cycle = current_cycle + LATENCY;
            if (head_entry.type == TRANSLATION) {
                head_entry.event_cycle += knob::translation_extra_latency;
            }
            head_entry.started_latency = 1;
        }

        // handle the oldest entry
        if ((RQ->peek().event_cycle <= current_cycle) && (RQ->occupancy > 0)) 
        {
            int index = RQ->get_head();
            PACKET& rq_entry = RQ->get_entry(RQ->get_head());

            if (cache_type == IS_STLB && knob::ideal_stlb)
            {
                uint64_t vpage = rq_entry.full_addr >> LOG2_PAGE_SIZE;
                uint64_t pframe = 0;
                auto it = buddy_allocator.vpage_to_pframe.find(vpage);
                if (it != buddy_allocator.vpage_to_pframe.end()) {
                    pframe = it->second;
                } else {
                    uint64_t paddr = buddy_allocator.access();
                    pframe = paddr >> LOG2_PAGE_SIZE;
                    buddy_allocator.map_vpage_to_pframe(vpage, pframe);
                }
                
                rq_entry.data = pframe;
                rq_entry.hit_where = hit_where_t::STLB;
                
                if (rq_entry.fill_level < fill_level)
                {
                    if (fill_level == FILL_L2)
                    {
                        if (rq_entry.fill_l1i)
                            upper_level_icache[read_cpu]->return_data(&rq_entry);
                        if (rq_entry.fill_l1d)
                            upper_level_dcache[read_cpu]->return_data(&rq_entry);
                    }
                    else
                    {
                        if (rq_entry.instruction)
                            upper_level_icache[read_cpu]->return_data(&rq_entry);
                        if (rq_entry.is_data)
                            upper_level_dcache[read_cpu]->return_data(&rq_entry);
                    }
                }
                
                uint64_t deque_cycle = current_core_cycle[read_cpu];
                if (deque_cycle >= rq_entry.enque_cycle[cache_type][IS_RQ]) {
                    service_time_hist.update(deque_cycle - rq_entry.enque_cycle[cache_type][IS_RQ]);
                }

                HIT[rq_entry.type]++;
                ACCESS[rq_entry.type]++;
                
                RQ->remove_queue(&rq_entry, deque_cycle);
                reads_available_this_cycle--;
                continue;
            }

            if (cache_type == IS_LLC && knob::ideal_llc_trans && rq_entry.type == TRANSLATION && rq_entry.ptw_level == 0)
            {
                uint64_t shadow_val = 0;
                bool is_pf=false, is_fa=false;
                bool tracked = buddy_allocator.shadow_get_entry(rq_entry.full_addr, 0, shadow_val, is_pf, is_fa);
                
                uint64_t pte_data = 0;
                uint64_t tagged_vpage = (rq_entry.virt_addr >> LOG2_PAGE_SIZE);
                
                if (!tracked || is_pf) {
                    auto it = buddy_allocator.vpage_to_pframe.find(tagged_vpage);
                    if (it != buddy_allocator.vpage_to_pframe.end()) {
                        pte_data = it->second << LOG2_PAGE_SIZE;
                    } else {
                        pte_data = buddy_allocator.access();
                        buddy_allocator.map_vpage_to_pframe(tagged_vpage, pte_data >> LOG2_PAGE_SIZE);
                    }
                    
                    if (!tracked) {
                        buddy_allocator.shadow_init_page(rq_entry.full_addr >> LOG2_PAGE_SIZE, 0);
                    }
                    buddy_allocator.shadow_set_entry(rq_entry.full_addr, 0, pte_data, PTEStatus::NO_FAULT);
                } else {
                    pte_data = shadow_val;
                }
                
                rq_entry.data = pte_data;
                rq_entry.hit_where = hit_where_t::LLC;
                
                if (upper_level_dcache[read_cpu] != NULL) {
                    upper_level_dcache[read_cpu]->return_data(&rq_entry);
                }
                
                uint64_t deque_cycle = uncore.cycle;
                if (deque_cycle >= rq_entry.enque_cycle[cache_type][IS_RQ]) {
                    service_time_hist.update(deque_cycle - rq_entry.enque_cycle[cache_type][IS_RQ]);
                }
                RQ->remove_queue(&rq_entry, deque_cycle);
                reads_available_this_cycle--;
                continue;
            }

            // cout <<NAME<< ",read,cpu,"<<read_cpu<<",addr,"<<hex2str(rq_entry.address)<<",vaddr,"<<hex2str(rq_entry.virt_addr)<<",lvl,"<<rq_entry.ptw_level<<'\n'; 

            cache_logger.log("handle_read", NAME, "addr", hex2str(rq_entry.address), "instr", rq_entry.instr_id, "type", +rq_entry.type, "current-cy", current_core_cycle[rq_entry.cpu], '\n');

            // access cache
            uint32_t set = get_set(rq_entry.address);
            int way = check_hit(&rq_entry);

            // pravesh: shadowSTLB
            bool shadow_hit = false;
            int shadow_way = -1;
            uint32_t shadow_set = 0;
            if (cache_type == IS_STLB) {
                uint64_t vpn = rq_entry.address;
                uint64_t newVPN = vpn >> 2;
                shadow_set = newVPN % ooo_cpu[read_cpu].shadowSTLB.NUM_SET;
                for (uint32_t w = 0; w < ooo_cpu[read_cpu].shadowSTLB.NUM_WAY; w++) {
                    if (ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][w].valid &&
                        ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][w].tag == newVPN) {
                        shadow_way = w;
                        break;
                    }
                }
                
                if (knob::shadowstlb_mode == "analysis") {
                    if (shadow_way != -1) {
                        // pravesh: shadowSTLB
                        ooo_cpu[read_cpu].shadowSTLB.lru_update(shadow_set, shadow_way);
                        shadow_hit = true;
                        
                        uint32_t idx = vpn & 3;
                        ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_used[idx] = true;
                        
                        if (way < 0 && !ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_pfs[idx]) {
                            ooo_cpu[read_cpu].shadowSTLB.shadow_stats.stlb_miss_saved_by_shadowSTLB++;
                        }
                    } else {
                        // pravesh: shadowSTLB
                        // Upon miss, directly do softlookup and fill its entries
                        for (uint32_t w = 0; w < ooo_cpu[read_cpu].shadowSTLB.NUM_WAY; w++) {
                            if (!ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][w].valid) {
                                shadow_way = w;
                                break;
                            }
                        }
                        if (shadow_way == -1) {
                            for (uint32_t w = 0; w < ooo_cpu[read_cpu].shadowSTLB.NUM_WAY; w++) {
                                if (ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][w].lru == ooo_cpu[read_cpu].shadowSTLB.NUM_WAY - 1) {
                                    shadow_way = w;
                                    break;
                                }
                            }
                        }
                        
                        if (shadow_way != -1) {
                            if (ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].valid) {
                                int used_count = 0;
                                for (int i = 0; i < 4; i++) {
                                    if (ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_used[i]) {
                                        used_count++;
                                    }
                                }
                                ooo_cpu[read_cpu].shadowSTLB.shadow_stats.shadow_block_footprint_hist[used_count]++;
                            }
                            
                            PTWclass* ptw = (PTWclass*)ooo_cpu[read_cpu].page_table_walker;
                            if (ptw != nullptr) {
                                ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].valid = true;
                                ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].tag = newVPN;
                                ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].address = newVPN;
                                
                                for (int i = 0; i < 4; i++) {
                                    uint64_t vpn_i = (newVPN << 2) | i;
                                    uint64_t vaddr_i = vpn_i << 12;
                                    uint64_t pte_val = 0;
                                    bool is_pf = false;
                                    ptw->soft_lookup_pte(vaddr_i, rq_entry.asid[1], pte_val, is_pf);
                                    
                                    ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_ptes[i] = pte_val;
                                    ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_pfs[i] = is_pf;
                                    ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_used[i] = false;
                                }
                                ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_used[vpn & 3] = true;
                                ooo_cpu[read_cpu].shadowSTLB.lru_update(shadow_set, shadow_way);
                            }
                        }
                    }
                } else {
                    // pravesh: shadowSTLB
                    // Detail mode: shadowSTLB replaces STLB
                    way = -1; // Ignore regular STLB check
                    if (shadow_way != -1) {
                        uint32_t idx = vpn & 3;
                        if (!ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_pfs[idx]) {
                            // Hit in shadowSTLB! Populate STLB block to satisfy subsequent simulator pipelines safely.
                            ooo_cpu[read_cpu].shadowSTLB.lru_update(shadow_set, shadow_way);
                            
                            ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_used[idx] = true;
                            
                            // Use same way in STLB to store this translation block on hit
                            block[set][shadow_way].valid = true;
                            block[set][shadow_way].tag = rq_entry.address;
                            block[set][shadow_way].address = rq_entry.address;
                            block[set][shadow_way].full_addr = rq_entry.full_addr;
                            block[set][shadow_way].data = ooo_cpu[read_cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_ptes[idx] >> 12;
                            lru_update(set, shadow_way);
                            
                            way = shadow_way;
                            shadow_hit = true;
                        }
                    }
                }
            }

            // Pravesh: if hit, check 8-byte entry there or not
            if (knob::enable_ptw && rq_entry.type == TRANSLATION && rq_entry.from_ptw && way >= 0)
            {
                uint64_t shadow_val;
                bool is_pf=false, is_fa=false;
                bool tracked = buddy_allocator.shadow_get_entry(rq_entry.full_addr, (uint8_t)rq_entry.ptw_level, shadow_val, is_pf, is_fa);

                if (is_pf) {
                    // // Entry was initialised but never officially walked by PTW yet.
                    // // Force a miss so the request goes to memory and gets the
                    // // correct PTE value rather than stale cached data.
                    // l.log(NAME, "handle_read", "PTW_FORCED_MISS_PAGE_FAULT",
                    //     "addr", hex2str(rq_entry.full_addr), "lvl", rq_entry.ptw_level, '\n');
                    way = -1; // override: treat as miss
                } else {
                    // PTW has officially walked this entry — use shadow value.
                    if (tracked) {
                        assert(shadow_val != 0
                            && "shadow page-table entry is zero on cache hit "
                               "(pte_paddr hit but shadow entry was never initialised or was zeroed)");
                        rq_entry.data = shadow_val;
                    }
                    // if(cache_type == IS_LLC) rq_entry.hit_where = hit_where_t::LLC;
                    // else if(cache_type == IS_L2C) rq_entry.hit_where = hit_where_t::L2C;
                    // else if(cache_type == IS_L1D) rq_entry.hit_where = hit_where_t::L1D;
                    // if (ooo_cpu[read_cpu].page_table_walker != NULL)
                    //     ooo_cpu[read_cpu].page_table_walker->handle_memory_response(&rq_entry);
                }
            }
            
            if (way >= 0) // read hit
            {
                // Update footprint on hit
                if (block[set][way].footprint.track_footprint) {
                    uint32_t word_num = (rq_entry.full_addr >> 3) & 0x7;
                    uint8_t word_bit = (1 << word_num);
                    if ((block[set][way].footprint.footprint & word_bit) == 0) {
                        block[set][way].footprint.footprint |= word_bit;
                        if (block[set][way].lru > block[set][way].footprint.max_lru_footprint_change) {
                            block[set][way].footprint.max_lru_footprint_change = block[set][way].lru;
                        }
                        block[set][way].footprint.footprint_changed = true;
                    } else {
                        block[set][way].footprint.entry_reuse[word_num]++;
                    }
                }

                // cout <<NAME<< ",hit,cpu,"<<read_cpu<<",addr,"<<hex2str(rq_entry.address)<<",vaddr,"<<hex2str(rq_entry.virt_addr)<<",lvl,"<<rq_entry.ptw_level<<'\n'; 
                
                rq_entry.hit_where = assign_hit_where(cache_type, 0); // read hit
                add_history_event(current_core_cycle[read_cpu], rq_entry.instr_id, rq_entry.virt_addr, rq_entry.address, rq_entry.full_addr, rq_entry.type, "CACHE_HIT", NAME.c_str(), false, true, false, false, 0, rq_entry.hit_where);

                if (cache_type == IS_ITLB) 
                {
                    rq_entry.instruction_pa = block[set][way].data;
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                    {
                        PROCESSED.add_queue(&rq_entry, current_core_cycle[read_cpu]);
                    }
                }
                else if (cache_type == IS_DTLB)
                {
                    rq_entry.data_pa = block[set][way].data;
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                    {
                        PROCESSED.add_queue(&rq_entry, current_core_cycle[read_cpu]);
                    }
                }
                else if (cache_type == IS_STLB) 
                {
                    rq_entry.data = block[set][way].data;
                }
                else if (cache_type == IS_L1I) 
                {
                    if (PROCESSED.occupancy < PROCESSED.SIZE)
                    {
                        PROCESSED.add_queue(&rq_entry, current_core_cycle[read_cpu]);
                    }
                }
                //else if (cache_type == IS_L1D) {
                else 
                {
                    // do not forward translation packets back to the core
                    if ((cache_type == IS_L1D) && (rq_entry.type != PREFETCH) && !rq_entry.tlb_access) {
                        if (PROCESSED.occupancy < PROCESSED.SIZE)
                        {
                            PROCESSED.add_queue(&rq_entry, current_core_cycle[read_cpu]);
                        }
                    }
                }

                // update prefetcher on load instruction
		        if (rq_entry.type == LOAD) 
                {
		            if(cache_type == IS_L1I)
                    {
		                l1i_prefetcher_cache_operate(read_cpu, rq_entry.ip, 1, block[set][way].prefetch);
                    }
                    if (cache_type == IS_L1D) 
                    {
		                l1d_prefetcher_operate(rq_entry.full_addr, rq_entry.ip, 1, rq_entry.type);
                    }
                    else if (cache_type == IS_L2C)
                    {
		                l2c_prefetcher_operate(block[set][way].address<<LOG2_BLOCK_SIZE, rq_entry.ip, 1, rq_entry.type, 0);
                    }
                    else if (cache_type == IS_LLC)
                    {
                        cpu = read_cpu;
                        llc_prefetcher_operate(block[set][way].address<<LOG2_BLOCK_SIZE, rq_entry.ip, 1, rq_entry.type, 0);
                        cpu = 0;
                    }
                }

                track_max_lru_on_hit(set, way);

                // update replacement policy
                if (cache_type == IS_LLC) 
                {
                    llc_update_replacement_state(read_cpu, set, way, block[set][way].full_addr, rq_entry.ip, 0, rq_entry.type, 1);
                }
                else
                {
                    update_replacement_state(read_cpu, set, way, block[set][way].full_addr, rq_entry.ip, 0, rq_entry.type, 1);
                }

                // COLLECT STATS
                sim_hit[read_cpu][rq_entry.type]++;
                sim_access[read_cpu][rq_entry.type]++;

                // @RBERA: this is a load hit (actually, this can be a RFO hit also!)
                // lokup the dependency chain of the load and popoluate the metadata
                block[set][way].reuse[rq_entry.type]++;
                if(rq_entry.is_data && rq_entry.type == LOAD)
                    block[set][way].reuse_frontal_dorsal[ooo_cpu[read_cpu].rob_pos_get_part_type(rq_entry.rob_position)]++;
                if(knob::track_load_hit_dependency_in_cache)
                {
                    vector<uint32_t> cat_dependents;
                    cat_dependents.resize(DEP_INSTR_TYPES, 0);
                    block[set][way].dependents += ooo_cpu[read_cpu].count_dependency(rq_entry.rob_index, cat_dependents);
                    for(uint32_t i = 0; i < DEP_INSTR_TYPES; ++i) block[set][way].cat_dependents[i] = cat_dependents[i];
                    // if(warmup_complete[read_cpu] && cache_type == IS_LLC && rq_entry.type == LOAD && dependents == 0)
                    // {
                    //     ooo_cpu[read_cpu].debug_dependents(rq_entry.rob_index);
                    //     assert(0);
                    // }
                }

                // RBERA-TODO: Dump cache access trace
                if(knob::l2c_dump_access_trace && cache_type == IS_L2C && warmup_complete[read_cpu])
                {
                    tracer.record_trace(rq_entry.full_addr, rq_entry.type, true);
                }
                if(knob::llc_dump_access_trace && cache_type == IS_LLC && warmup_complete[read_cpu])
                {
                    tracer.record_trace(rq_entry.full_addr, rq_entry.type, true);
                }

                // check fill level
                if (rq_entry.fill_level < fill_level) 
                {
                    if(fill_level == FILL_L2)
                    {
                        if(rq_entry.fill_l1i)
                        {
                            upper_level_icache[read_cpu]->return_data(&rq_entry);
                        }
                        if(rq_entry.fill_l1d)
                        {
                            upper_level_dcache[read_cpu]->return_data(&rq_entry);
                        }
                    }
		            else
                    {
                        if (rq_entry.instruction)
                        {
                            upper_level_icache[read_cpu]->return_data(&rq_entry);
                        }
                        if (rq_entry.is_data)
                        {
                            upper_level_dcache[read_cpu]->return_data(&rq_entry);
                        }
                    }
                }

                // update prefetch stats and reset prefetch bit
                if (block[set][way].prefetch) 
                {
                    pf_useful++;
                    block[set][way].prefetch = 0;
                }
                block[set][way].used = 1;

                // Deadblock pred: store prediction in block on hit, increment usage, train
                if (cache_type == IS_LLC && rq_entry.type == LOAD && knob::knob_doa_predictor && llc_pred_perc != NULL)
                {
                    block[set][way].usage++; // Deadblock pred: increment usage on each hit
                }

                HIT[rq_entry.type]++;
                ACCESS[rq_entry.type]++;
                
                // remove this entry from RQ
                uint64_t deque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[read_cpu];
                if (deque_cycle >= rq_entry.enque_cycle[cache_type][IS_RQ]) {
                    service_time_hist.update(deque_cycle - rq_entry.enque_cycle[cache_type][IS_RQ]);
                }
                RQ->remove_queue(&rq_entry, deque_cycle);
		        reads_available_this_cycle--;
            }
            else // read miss
            {
                // if(cache_type == IS_STLB){
                //     cout <<"cy, " <<dec<< current_core_cycle[read_cpu] << " , addr, " << hex << RQ->peek().address << ", fulladdr, " << RQ->peek().full_addr << ", t, " << +RQ->peek().instruction << ", " << dec << endl;
                // }

                add_history_event(current_core_cycle[read_cpu], rq_entry.instr_id, rq_entry.virt_addr, rq_entry.address, rq_entry.full_addr, rq_entry.type, "CACHE_MISS", NAME.c_str(), false, false, true, false, 0, rq_entry.hit_where);
                DP ( if (warmup_complete[read_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " read miss";
                cout << " instr_id: " << rq_entry.instr_id << " address: " << hex << rq_entry.address;
                cout << " full_addr: " << rq_entry.full_addr << dec;
                cout << " cycle: " << rq_entry.event_cycle << endl; });

                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&rq_entry);

		        if(mshr_index == -2)
                {
                    // this is a data/instruction collision in the MSHR, so we have to wait before we can allocate this miss
                    miss_handled = 0;
                }
                else if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) // this is a new miss
                { 
		            if(cache_type == IS_LLC)
        		    {
		                // check to make sure the DRAM RQ has room for this LLC read miss
		                if (lower_level->get_occupancy(1, rq_entry.address) == lower_level->get_size(1, rq_entry.address))
                        {
                            miss_handled = 0;
                            // STALL[rq_entry.type]++; ??pravesh
                        }
                        else
                        {
                            rq_entry.hit_where = hit_where_t::DRAM; // LLC miss => DRAM hit
                            if(lower_level)
                            {
                                // pravesh
                                record_offchip_event(read_cpu, &rq_entry);
                            }
                            add_mshr(&rq_entry);
                            if(lower_level)
                            {
                                lower_level->add_rq(&rq_entry);
                                
                                // @RBERA: if this is a data load missing LLC, then:
                                // 1. Monitor the position of the load in ROB
                                // 2. Send signal to LQ and ROB about this miss
                                if(rq_entry.is_data && rq_entry.type == LOAD)
                                {
                                    if(rq_entry.rob_position < 0 || rq_entry.rob_position >= ROB_SIZE)
                                    {
                                        cerr << "[" << NAME << "_ERROR] invalid ROB position: index=" << index << " pos=" << rq_entry.rob_position;
                                        cerr << " addr=0x" << hex << rq_entry.address << " full_addr=0x" << rq_entry.full_addr;
                                        cerr << " instr_id=" << dec << rq_entry.instr_id << " type=" << +rq_entry.type << endl;
                                        assert(0 && "ROB position out of bounds for LLC load miss");
                                    }
                                    missing_load_rob_pos_hist[rq_entry.rob_position]++;
                                    send_signal_to_core(read_cpu, rq_entry);
                                }
                            }
                        }
		            }
                    else
                    {
                        // STLB+PTW: initiate walk BEFORE allocating MSHR to prevent orphaned entries.
                        // If PTW queue is full and we allocated MSHR first, on retry the MSHR hit path
                        // would merge the packet without re-triggering PTW, leaving the MSHR stuck.
                        if (cache_type == IS_STLB && !lower_level &&
                            knob::enable_ptw && ooo_cpu[read_cpu].page_table_walker != NULL)
                        {
                            bool success = ooo_cpu[read_cpu].page_table_walker->initiate_page_walk(&rq_entry, rq_entry.full_addr);
                            if (success) {
                                add_mshr(&rq_entry);
                            } else {
                                miss_handled = 0;
                            }
                        }
                        else
                        {
                        
                        // add it to the next level's read queue
                        if (lower_level)
                        {
                            if (lower_level->get_occupancy(1, rq_entry.address) == lower_level->get_size(1, rq_entry.address))
                            {
                                miss_handled = 0;
                                STALL[rq_entry.type]++; //??pravesh
                            }
                            else
                            {
                                // add it to mshr (read miss)
                                add_mshr(&rq_entry);
                                lower_level->add_rq(&rq_entry);
                            }

                        }
                        else // this is the last level (STLB with PTW disabled or fallback)
                        {
                            if (cache_type == IS_STLB)
                            {
                                // add it to mshr (read miss)
                                add_mshr(&rq_entry);
                                uint64_t pa = va_to_pa(rq_entry.asid[1], rq_entry.instr_id, rq_entry.full_addr, rq_entry.address, 0);
                                rq_entry.data = pa >> LOG2_PAGE_SIZE;
                                rq_entry.event_cycle = current_core_cycle[read_cpu];
                                rq_entry.hit_where = hit_where_t::PTW; // STLB miss => PTW
                                return_data(&rq_entry);
                            }
                        }
                        } // end else (non-STLB-PTW path)
                    }
                }
                else 
                {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) // not enough MSHR resource
                    {
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[rq_entry.type]++;
                    }
                    else if (mshr_index != -1)  // already in-flight miss
                    {
                        // cout <<NAME<< ",merge,cpu,"<<read_cpu<<",addr,"<<hex2str(rq_entry.address)<<",vaddr,"<<hex2str(rq_entry.virt_addr)<<",lvl,"<<rq_entry.ptw_level<<'\n'; 

                        rq_entry.hit_where = assign_hit_where(cache_type, 3); // MSHR hit
                        add_history_event(current_core_cycle[read_cpu], rq_entry.instr_id, rq_entry.virt_addr, rq_entry.address, rq_entry.full_addr, rq_entry.type, "MSHR_MERGE", NAME.c_str(), false, false, false, true, MSHR.entry[mshr_index].instr_id, rq_entry.hit_where);
                        
                        // if (rq_entry.type == TRANSLATION) {
                        //     l.log(NAME, "handle_read", "MERGE",
                        //         "addr", hex2str(rq_entry.address),
                        //         "mshr_idx", mshr_index, '\n');
                        // }

                        // mark merged consumer
                        if (rq_entry.type == RFO) 
                        {
                            if (rq_entry.tlb_access) 
                            {
                                uint32_t sq_index = rq_entry.sq_index;
                                MSHR.entry[mshr_index].store_merged = 1;
                                MSHR.entry[mshr_index].sq_index_depend_on_me.insert (sq_index);
				                MSHR.entry[mshr_index].sq_index_depend_on_me.join(rq_entry.sq_index_depend_on_me, SQ_SIZE);
                            }
                            if (rq_entry.load_merged) 
                            {
                                //uint32_t lq_index = rq_entry.lq_index; 
                                MSHR.entry[mshr_index].load_merged = 1;
                                //MSHR.entry[mshr_index].lq_index_depend_on_me[lq_index] = 1;
				                MSHR.entry[mshr_index].lq_index_depend_on_me.join(rq_entry.lq_index_depend_on_me, LQ_SIZE);
                            }
                        }
                        else 
                        {
                            if (rq_entry.instruction) 
                            {
                                uint32_t rob_index = rq_entry.rob_index;
                                MSHR.entry[mshr_index].instruction = 1; // add as instruction type
                                MSHR.entry[mshr_index].instr_merged = 1;
                                MSHR.entry[mshr_index].rob_index_depend_on_me.insert(rob_index);

                                DP (if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                cout << " merged rob_index: " << rob_index << " instr_id: " << rq_entry.instr_id << endl; });

                                if (rq_entry.instr_merged) 
                                {
				                    MSHR.entry[mshr_index].rob_index_depend_on_me.join(rq_entry.rob_index_depend_on_me, ROB_SIZE);
                                    DP (if (warmup_complete[MSHR.entry[mshr_index].cpu]) {
                                    cout << "[INSTR_MERGED] " << __func__ << " cpu: " << MSHR.entry[mshr_index].cpu << " instr_id: " << MSHR.entry[mshr_index].instr_id;
                                    cout << " merged rob_index: " << i << " instr_id: N/A" << endl; });
                                }
                            }
                            else 
                            {
                                uint32_t lq_index = rq_entry.lq_index;
                                MSHR.entry[mshr_index].is_data = 1; // add as data type
                                MSHR.entry[mshr_index].load_merged = 1;
                                MSHR.entry[mshr_index].lq_index_depend_on_me.insert(lq_index);

                                DP (if (warmup_complete[read_cpu]) {
                                cout << "[DATA_MERGED] " << __func__ << " cpu: " << read_cpu << " instr_id: " << rq_entry.instr_id;
                                cout << " merged rob_index: " << rq_entry.rob_index << " instr_id: " << rq_entry.instr_id << " lq_index: " << rq_entry.lq_index << endl; });

				                MSHR.entry[mshr_index].lq_index_depend_on_me.join(rq_entry.lq_index_depend_on_me, LQ_SIZE);
                                if (rq_entry.store_merged) 
                                {
                                    MSHR.entry[mshr_index].store_merged = 1;
				                    MSHR.entry[mshr_index].sq_index_depend_on_me.join(rq_entry.sq_index_depend_on_me, SQ_SIZE);
                                }
                            }
                        }

                        // update fill_level
                        if (rq_entry.fill_level < MSHR.entry[mshr_index].fill_level)
                        {
                            MSHR.entry[mshr_index].fill_level = rq_entry.fill_level;
                        }
			            if((rq_entry.fill_l1i) && (MSHR.entry[mshr_index].fill_l1i != 1))
                        {
                            MSHR.entry[mshr_index].fill_l1i = 1;
                        }
            			if((rq_entry.fill_l1d) && (MSHR.entry[mshr_index].fill_l1d != 1))
                        {
                            MSHR.entry[mshr_index].fill_l1d = 1;
                        }

                        // update request
                        if (MSHR.entry[mshr_index].type == PREFETCH) 
                        {
                            //RBERA: why calling it late even in case of prefetch request hitting another in-flight prefetch?
							pf_late++;
                            uint8_t  prior_returned = MSHR.entry[mshr_index].returned;
                            uint64_t prior_event_cycle = MSHR.entry[mshr_index].event_cycle;
                            MSHR.entry[mshr_index] = rq_entry;
                            
                            // in case request is already returned, we should keep event_cycle and retunred variables
                            MSHR.entry[mshr_index].returned = prior_returned;
                            MSHR.entry[mshr_index].event_cycle = prior_event_cycle;
                        }

                        MSHR_MERGED[rq_entry.type]++;

                        DP ( if (warmup_complete[read_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << rq_entry.instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << rq_entry.address;
                        cout << " full_addr: " << rq_entry.full_addr << dec;
                        cout << " cycle: " << rq_entry.event_cycle << endl; });
                    }
                    else // WE SHOULD NOT REACH HERE
                    {
                        cerr << "[" << NAME << "] RQ MSHR errors: addr=0x" << hex << rq_entry.address
                             << " full_addr=0x" << rq_entry.full_addr << dec
                             << " instr_id=" << rq_entry.instr_id << " type=" << +rq_entry.type << endl;
                        assert(0 && "RQ MSHR processing error");
                    }
                }

                if (miss_handled) 
                {
                    // update prefetcher on load instruction
		            if (rq_entry.type == LOAD) 
                    {
		                if(cache_type == IS_L1I)
                        {
			                l1i_prefetcher_cache_operate(read_cpu, rq_entry.ip, 0, 0);
                        }
                        if (cache_type == IS_L1D) 
                        {
                            l1d_prefetcher_operate(rq_entry.full_addr, rq_entry.ip, 0, rq_entry.type);
                        }
                        if (cache_type == IS_L2C)
                        {
			                l2c_prefetcher_operate(rq_entry.address<<LOG2_BLOCK_SIZE, rq_entry.ip, 0, rq_entry.type, 0);
                        }
                        if (cache_type == IS_LLC)
                        {
                            cpu = read_cpu;
                            llc_prefetcher_operate(rq_entry.address<<LOG2_BLOCK_SIZE, rq_entry.ip, 0, rq_entry.type, 0);
                            cpu = 0;
                        }
                    }

                    MISS[rq_entry.type]++;
                    ACCESS[rq_entry.type]++;

                    // remove this entry from RQ
                    uint64_t deque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[read_cpu];
                    RQ->remove_queue(&rq_entry, deque_cycle);
		            reads_available_this_cycle--;
                }
            }
        }
	    else
	    {
	        return;
	    }

	    if(reads_available_this_cycle == 0)
	    {
	        return;
	    }
    }
}

// =====================================================================
// PREFETCH OPERATIONS - Handle prefetch requests
// =====================================================================

void CACHE::handle_prefetch()
{
    // handle prefetch
    for (uint32_t i=0; i<MAX_READ; i++) 
    {  
        uint32_t prefetch_cpu = PQ.entry[PQ.head].cpu;
        if (prefetch_cpu == NUM_CPUS)
        {
            return;
        }

        // handle the oldest entry
        if ((PQ.entry[PQ.head].event_cycle <= current_core_cycle[prefetch_cpu]) && (PQ.occupancy > 0)) 
        {
            int index = PQ.head;

            // access cache
            uint32_t set = get_set(PQ.entry[index].address);
            int way = check_hit(&PQ.entry[index]);
            
            if (way >= 0) // prefetch hit
            { 
                // Update footprint on hit
                if (block[set][way].footprint.track_footprint) {
                    uint32_t word_num = (PQ.entry[index].full_addr >> 3) & 0x7;
                    uint8_t word_bit = (1 << word_num);
                    if ((block[set][way].footprint.footprint & word_bit) == 0) {
                        block[set][way].footprint.footprint |= word_bit;
                        if (block[set][way].lru > block[set][way].footprint.max_lru_footprint_change) {
                            block[set][way].footprint.max_lru_footprint_change = block[set][way].lru;
                        }
                        block[set][way].footprint.footprint_changed = true;
                    } else {
                        block[set][way].footprint.entry_reuse[word_num]++;
                    }
                }

                PQ.entry[index].hit_where = assign_hit_where(cache_type, 0); // prefetch hit

                track_max_lru_on_hit(set, way);

                // update replacement policy
                if (cache_type == IS_LLC) 
                {
                    llc_update_replacement_state(prefetch_cpu, set, way, block[set][way].full_addr, PQ.entry[index].ip, 0, PQ.entry[index].type, 1);
                }
                else
                {
                    update_replacement_state(prefetch_cpu, set, way, block[set][way].full_addr, PQ.entry[index].ip, 0, PQ.entry[index].type, 1);
                }

                // COLLECT STATS
                sim_hit[prefetch_cpu][PQ.entry[index].type]++;
                sim_access[prefetch_cpu][PQ.entry[index].type]++;

                // @RBERA: populate reuse metadata
                block[set][way].reuse[PQ.entry[index].type]++;

                // RBERA-TODO: Dump cache access trace
                if(knob::l2c_dump_access_trace && cache_type == IS_L2C && warmup_complete[prefetch_cpu])
                {
                    tracer.record_trace(PQ.entry[index].full_addr, PQ.entry[index].type, true);
                }
                if(knob::llc_dump_access_trace && cache_type == IS_LLC && warmup_complete[prefetch_cpu])
                {
                    tracer.record_trace(PQ.entry[index].full_addr, PQ.entry[index].type, true);
                }

		        // run prefetcher on prefetches from higher caches
		        if(PQ.entry[index].pf_origin_level < fill_level)
		        {
		            if (cache_type == IS_L1D)
		                l1d_prefetcher_operate(PQ.entry[index].full_addr, PQ.entry[index].ip, 1, PREFETCH);
                    else if (cache_type == IS_L2C)
                      PQ.entry[index].pf_metadata = l2c_prefetcher_operate(block[set][way].address<<LOG2_BLOCK_SIZE, PQ.entry[index].ip, 1, PREFETCH, PQ.entry[index].pf_metadata);
                    else if (cache_type == IS_LLC)
    		        {
                        cpu = prefetch_cpu;
                        PQ.entry[index].pf_metadata = llc_prefetcher_operate(block[set][way].address<<LOG2_BLOCK_SIZE, PQ.entry[index].ip, 1, PREFETCH, PQ.entry[index].pf_metadata);
                        cpu = 0;
		            }
		        }

                // check fill level
                if (PQ.entry[index].fill_level < fill_level) 
                {
                    if(fill_level == FILL_L2)
                    {
                        if(PQ.entry[index].fill_l1i)
                        {
                            upper_level_icache[prefetch_cpu]->return_data(&PQ.entry[index]);
                        }
                        if(PQ.entry[index].fill_l1d)
                        {
                            upper_level_dcache[prefetch_cpu]->return_data(&PQ.entry[index]);
                        }
		            }
		            else
		            {
                        if (PQ.entry[index].instruction)
                        {
                            upper_level_icache[prefetch_cpu]->return_data(&PQ.entry[index]);
                        }
                        if (PQ.entry[index].is_data)
                        {
                            upper_level_dcache[prefetch_cpu]->return_data(&PQ.entry[index]);
                        }
		            }
                }

                HIT[PQ.entry[index].type]++;
                ACCESS[PQ.entry[index].type]++;
                
                // remove this entry from PQ
                uint64_t deque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[prefetch_cpu];
                PQ.remove_queue(&PQ.entry[index], deque_cycle);
		        reads_available_this_cycle--;
            }
            else // prefetch miss
            {
                DP ( if (warmup_complete[prefetch_cpu]) {
                cout << "[" << NAME << "] " << __func__ << " prefetch miss";
                cout << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << PQ.entry[index].fill_level;
                cout << " cycle: " << PQ.entry[index].event_cycle << endl; });

                // check mshr
                uint8_t miss_handled = 1;
                int mshr_index = check_mshr(&PQ.entry[index]);

		        if(mshr_index == -2)
                {
                    // this is a data/instruction collision in the MSHR, so we have to wait before we can allocate this miss
                    miss_handled = 0;
                }
                else if ((mshr_index == -1) && (MSHR.occupancy < MSHR_SIZE)) // this is a new miss
                {
                    DP ( if (warmup_complete[PQ.entry[index].cpu]) {
                    cout << "[" << NAME << "_PQ] " <<  __func__ << " want to add instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                    cout << " full_addr: " << PQ.entry[index].full_addr << dec;
                    cout << " occupancy: " << lower_level->get_occupancy(3, PQ.entry[index].address) << " SIZE: " << lower_level->get_size(3, PQ.entry[index].address) << endl; });

                    // first check if the lower level PQ (or RQ, in case of LLC) is full or not.
                    // this is possible since multiple prefetchers can exist at each level of caches
                    if (lower_level) 
                    {
		                if (cache_type == IS_LLC) 
                        {
			                if (lower_level->get_occupancy(1, PQ.entry[index].address) == lower_level->get_size(1, PQ.entry[index].address))
                            {
			                    miss_handled = 0;
                            }
			                else 
                            {
			                    // run prefetcher on prefetches from higher caches
			                    if(PQ.entry[index].pf_origin_level < fill_level)
                                {
                                    if (cache_type == IS_LLC)
                                    {
                                        cpu = prefetch_cpu;
                                        PQ.entry[index].pf_metadata = llc_prefetcher_operate(PQ.entry[index].address<<LOG2_BLOCK_SIZE, PQ.entry[index].ip, 0, PREFETCH, PQ.entry[index].pf_metadata);
                                        cpu = 0;
				                    }
			                    }

			                    // add it to MSHRs if this prefetch miss will be filled to this cache level
			                    if (PQ.entry[index].fill_level <= fill_level)
                                {
			                        add_mshr(&PQ.entry[index]);
                                }

			                    lower_level->add_rq(&PQ.entry[index]); // add it to the DRAM RQ
			                }
		                }
		                else 
                        {
			                if (lower_level->get_occupancy(3, PQ.entry[index].address) == lower_level->get_size(3, PQ.entry[index].address))
                            {
			                    miss_handled = 0;
                            }
			                else 
                            {
			                    // run prefetcher on prefetches from higher caches
			                    if(PQ.entry[index].pf_origin_level < fill_level)
                                {
                                    if (cache_type == IS_L1D)
                                    {
                                        l1d_prefetcher_operate(PQ.entry[index].full_addr, PQ.entry[index].ip, 0, PREFETCH);
                                    }
                                    if (cache_type == IS_L2C)
                                    {
                                        PQ.entry[index].pf_metadata = l2c_prefetcher_operate(PQ.entry[index].address<<LOG2_BLOCK_SIZE, PQ.entry[index].ip, 0, PREFETCH, PQ.entry[index].pf_metadata);
                                    }
                                }
			  
			                    // add it to MSHRs if this prefetch miss will be filled to this cache level
			                    if (PQ.entry[index].fill_level <= fill_level)
                                {
			                        add_mshr(&PQ.entry[index]);
                                }

			                    lower_level->add_pq(&PQ.entry[index]); // add it to the PQ of lower cache
			                }
		                }
		            }
                }
                else 
                {
                    if ((mshr_index == -1) && (MSHR.occupancy == MSHR_SIZE)) // not enough MSHR resource
                    { 
                        // TODO: should we allow prefetching with lower fill level at this case?
                        
                        // cannot handle miss request until one of MSHRs is available
                        miss_handled = 0;
                        STALL[PQ.entry[index].type]++;
                    }
                    else if (mshr_index != -1)  // already in-flight miss
                    {
                        PQ.entry[index].hit_where = assign_hit_where(cache_type, 3); // prefetch hit in MSHR

                        // no need to update request except fill_level
                        // update fill_level
                        if (PQ.entry[index].fill_level < MSHR.entry[mshr_index].fill_level)
                        {
                            MSHR.entry[mshr_index].fill_level = PQ.entry[index].fill_level;
                        }

                        if((PQ.entry[index].fill_l1i) && (MSHR.entry[mshr_index].fill_l1i != 1))
                        {
                            MSHR.entry[mshr_index].fill_l1i = 1;
                        }
                        if((PQ.entry[index].fill_l1d) && (MSHR.entry[mshr_index].fill_l1d != 1))
                        {
                            MSHR.entry[mshr_index].fill_l1d = 1;
                        }

                        MSHR_MERGED[PQ.entry[index].type]++;

                        DP ( if (warmup_complete[prefetch_cpu]) {
                        cout << "[" << NAME << "] " << __func__ << " mshr merged";
                        cout << " instr_id: " << PQ.entry[index].instr_id << " prior_id: " << MSHR.entry[mshr_index].instr_id; 
                        cout << " address: " << hex << PQ.entry[index].address;
                        cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << MSHR.entry[mshr_index].fill_level;
                        cout << " cycle: " << MSHR.entry[mshr_index].event_cycle << endl; });
                    }
                    else // WE SHOULD NOT REACH HERE
                    { 
                        cerr << "[" << NAME << "] PQ MSHR errors: addr=0x" << hex << PQ.entry[index].address
                             << " full_addr=0x" << PQ.entry[index].full_addr << dec
                             << " instr_id=" << PQ.entry[index].instr_id << " type=" << +PQ.entry[index].type << endl;
                        assert(0 && "PQ MSHR processing error");
                    }
                }

                if (miss_handled) 
                {
                    DP ( if (warmup_complete[prefetch_cpu]) {
                    cout << "[" << NAME << "] " << __func__ << " prefetch miss handled";
                    cout << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
                    cout << " full_addr: " << PQ.entry[index].full_addr << dec << " fill_level: " << PQ.entry[index].fill_level;
                    cout << " cycle: " << PQ.entry[index].event_cycle << endl; });

                    MISS[PQ.entry[index].type]++;
                    ACCESS[PQ.entry[index].type]++;

                    // remove this entry from PQ
                    uint64_t deque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[prefetch_cpu];
                    PQ.remove_queue(&PQ.entry[index], deque_cycle);
		            reads_available_this_cycle--;
                }
            }
        }
	    else
	    {
	        return;
	    }

	    if(reads_available_this_cycle == 0)
        {
            return;
        }
    }
}

// =====================================================================
// MAIN SIMULATION CYCLE - Orchestrates all cache operations
// =====================================================================

void CACHE::operate()
{
    handle_fill();
    handle_writeback();
    reads_available_this_cycle = MAX_READ;
    handle_read();

    if (PQ.occupancy && (reads_available_this_cycle > 0))
        handle_prefetch();
}

// =====================================================================
// CACHE UTILITY FUNCTIONS - Address mapping, set/way computation
// =====================================================================

uint32_t CACHE::get_set(uint64_t address)
{
    return (uint32_t) (address & ((1 << lg2(NUM_SET)) - 1)); 
}

uint32_t CACHE::get_way(uint64_t address, uint32_t set)
{
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (block[set][way].valid && (block[set][way].tag == address)) 
            return way;
    }

    return NUM_WAY;
}

void CACHE::fill_cache(uint32_t set, uint32_t way, PACKET *packet)
{
    // pravesh: shadowSTLB
    if (cache_type == IS_STLB && knob::shadowstlb_mode == "detail") {
        uint64_t vpn = packet->address;
        uint64_t newVPN = vpn >> 2;
        uint32_t shadow_set = newVPN % ooo_cpu[packet->cpu].shadowSTLB.NUM_SET;
        int shadow_way = -1;
        for (uint32_t w = 0; w < ooo_cpu[packet->cpu].shadowSTLB.NUM_WAY; w++) {
            if (ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][w].valid &&
                ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][w].tag == newVPN) {
                shadow_way = w;
                break;
            }
        }
        
        if (shadow_way == -1) {
            for (uint32_t w = 0; w < ooo_cpu[packet->cpu].shadowSTLB.NUM_WAY; w++) {
                if (!ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][w].valid) {
                    shadow_way = w;
                    break;
                }
            }
            if (shadow_way == -1) {
                for (uint32_t w = 0; w < ooo_cpu[packet->cpu].shadowSTLB.NUM_WAY; w++) {
                    if (ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][w].lru == ooo_cpu[packet->cpu].shadowSTLB.NUM_WAY - 1) {
                        shadow_way = w;
                        break;
                    }
                }
            }
        }
        
        if (shadow_way != -1) {
            if (ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].valid) {
                int used_count = 0;
                for (int i = 0; i < 4; i++) {
                    if (ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_used[i]) {
                        used_count++;
                    }
                }
                ooo_cpu[packet->cpu].shadowSTLB.shadow_stats.shadow_block_footprint_hist[used_count]++;
            }
            
            PTWclass* ptw = (PTWclass*)ooo_cpu[packet->cpu].page_table_walker;
            if (ptw != nullptr) {
                ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].valid = true;
                ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].tag = newVPN;
                ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].address = newVPN;
                
                for (int i = 0; i < 4; i++) {
                    uint64_t vpn_i = (newVPN << 2) | i;
                    uint64_t vaddr_i = vpn_i << 12;
                    uint64_t pte_val = 0;
                    bool is_pf = false;
                    ptw->soft_lookup_pte(vaddr_i, packet->asid[1], pte_val, is_pf);
                    
                    ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_ptes[i] = pte_val;
                    ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_pfs[i] = is_pf;
                    ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_used[i] = false;
                }
                ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_used[vpn & 3] = true;
                ooo_cpu[packet->cpu].shadowSTLB.lru_update(shadow_set, shadow_way);
            }
        }
        return;
    }

#ifdef SANITY_CHECK
    if (cache_type == IS_ITLB) {
        if (packet->data == 0) {
            cerr << "[" << NAME << "_ERROR] ITLB fill with null data: addr=0x" << hex << packet->address;
            cerr << " full_addr=0x" << packet->full_addr << " instr_id=" << dec << packet->instr_id << endl;
            assert(0 && "ITLB packet data is null");
        }
    }

    if (cache_type == IS_DTLB) {
        if (packet->data == 0) {
            cerr << "[" << NAME << "_ERROR] DTLB fill with null data: addr=0x" << hex << packet->address;
            cerr << " full_addr=0x" << packet->full_addr << " instr_id=" << dec << packet->instr_id << endl;
            assert(0 && "DTLB packet data is null");
        }
    }

    if (cache_type == IS_STLB) {
        if (packet->data == 0) {
            cerr << "[" << NAME << "_ERROR] STLB fill with null data: addr=0x" << hex << packet->address;
            cerr << " full_addr=0x" << packet->full_addr << " instr_id=" << dec << packet->instr_id << endl;
            assert(0 && "STLB packet data is null");
        }
    }
#endif
    if (block[set][way].prefetch && (block[set][way].used == 0))
        pf_useless++;
    
    if(block[set][way].valid == 1) // eviction
    {
        /* call any routine before eviction */
        track_stats_from_victim(set, way);
    }

    if (block[set][way].valid == 0)
        block[set][way].valid = 1;
    block[set][way].dirty = 0;
    block[set][way].prefetch = (packet->type == PREFETCH) ? 1 : 0;
    block[set][way].used = 0;

    if (block[set][way].prefetch)
    {
        pf_filled++;
    }

    block[set][way].delta = packet->delta;
    block[set][way].depth = packet->depth;
    block[set][way].signature = packet->signature;
    block[set][way].confidence = packet->confidence;

    block[set][way].tag = packet->address;
    block[set][way].address = packet->address;
    block[set][way].full_addr = packet->full_addr;
    block[set][way].data = packet->data;
    block[set][way].ip = packet->ip;
    block[set][way].cpu = packet->cpu;
    block[set][way].instr_id = packet->instr_id;
    block[set][way].asid[0] = packet->asid[0];
    block[set][way].asid[1] = packet->asid[1];

    block[set][way].reset_metadata();
    block[set][way].fill_ip = packet->ip;

    block[set][way].footprint.packet_type = packet->type;
    block[set][way].footprint.ptw_level = packet->ptw_level;

    bool track = false;
    if (knob::footprint_track_type == "ALL") {
        track = true;
    } else if (knob::footprint_track_type == "LOAD" && packet->type == LOAD) {
        track = true;
    } else if (knob::footprint_track_type == "PREFETCH" && packet->type == PREFETCH) {
        track = true;
    } else if (knob::footprint_track_type == "TRANSLATION" && packet->type == TRANSLATION) {
        track = true;
    }
    block[set][way].footprint.track_footprint = track;

    block[set][way].footprint.footprint = 0;
    block[set][way].footprint.max_lru_footprint_change = 0;
    block[set][way].footprint.footprint_changed = false;

    DP ( if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "] " << __func__ << " set: " << set << " way: " << way;
    cout << " lru: " << block[set][way].lru << " tag: " << hex << block[set][way].tag << " full_addr: " << block[set][way].full_addr;
    cout << " data: " << block[set][way].data << dec << endl; });

    // pravesh: shadowSTLB
    if (cache_type == IS_STLB && knob::shadowstlb_mode != "analysis" && knob::shadowstlb_mode != "detail") {
        uint64_t vpn = packet->address;
        uint64_t newVPN = vpn >> 2;
        uint32_t shadow_set = newVPN % ooo_cpu[packet->cpu].shadowSTLB.NUM_SET;
        int shadow_way = -1;
        for (uint32_t w = 0; w < ooo_cpu[packet->cpu].shadowSTLB.NUM_WAY; w++) {
            if (ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][w].valid &&
                ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][w].tag == newVPN) {
                shadow_way = w;
                break;
            }
        }
        
        if (shadow_way == -1) {
            for (uint32_t w = 0; w < ooo_cpu[packet->cpu].shadowSTLB.NUM_WAY; w++) {
                if (!ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][w].valid) {
                    shadow_way = w;
                    break;
                }
            }
            if (shadow_way == -1) {
                for (uint32_t w = 0; w < ooo_cpu[packet->cpu].shadowSTLB.NUM_WAY; w++) {
                    if (ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][w].lru == ooo_cpu[packet->cpu].shadowSTLB.NUM_WAY - 1) {
                        shadow_way = w;
                        break;
                    }
                }
            }
        }
        
        if (shadow_way != -1) {
            PTWclass* ptw = (PTWclass*)ooo_cpu[packet->cpu].page_table_walker;
            if (ptw != nullptr) {
                ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].valid = true;
                ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].tag = newVPN;
                ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].address = newVPN;
                
                for (int i = 0; i < 4; i++) {
                    uint64_t vpn_i = (newVPN << 2) | i;
                    uint64_t vaddr_i = vpn_i << 12;
                    uint64_t pte_val = 0;
                    bool is_pf = false;
                    ptw->soft_lookup_pte(vaddr_i, packet->asid[1], pte_val, is_pf);
                    
                    ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_ptes[i] = pte_val;
                    ooo_cpu[packet->cpu].shadowSTLB.block[shadow_set][shadow_way].shadow_stlb_data.shadow_pfs[i] = is_pf;
                }
                ooo_cpu[packet->cpu].shadowSTLB.lru_update(shadow_set, shadow_way);
            }
        }
    }
}

int CACHE::check_hit(PACKET *packet)
{
    uint32_t set = get_set(packet->address);
    int match_way = -1;

    if (NUM_SET < set) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " invalid set index: " << set << " NUM_SET: " << NUM_SET;
        cerr << " address: " << hex << packet->address << " full_addr: " << packet->full_addr << dec;
        cerr << " event: " << packet->event_cycle << endl;
        assert(0 && "Set index exceeds NUM_SET boundary");
    }

    // perfect cache
    if((cache_type == IS_L1D && knob::l1d_perfect)
        || (cache_type == IS_L2C && knob::l2c_perfect)
        || (cache_type == IS_LLC && knob::llc_perfect))
    {
        match_way = 0;
        return match_way;
    }

    // hit
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (block[set][way].valid && (block[set][way].tag == packet->address)) {

            match_way = way;

            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "] " << __func__ << " instr_id: " << packet->instr_id << " type: " << +packet->type << hex << " addr: " << packet->address;
            cout << " full_addr: " << packet->full_addr << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
            cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru;
            cout << " event: " << packet->event_cycle << " cycle: " << current_core_cycle[cpu] << endl; });

            break;
        }
    }
    
    // pseudo-perfect LLC for missing frontal/dorsal loads
    if(packet->is_data && packet->type == LOAD    // this should only be modeled for data loads
        && match_way == -1                        // that are missing in the actual cache
        && (cache_type == IS_LLC && knob::llc_pseudo_perfect_enable))
    {
        stats.pseudo_perfect.data_load_misses++;

        if((knob::llc_pseudo_perfect_enable_frontal && ooo_cpu[packet->cpu].rob_pos_is_frontal(packet->rob_position))
            || (knob::llc_pseudo_perfect_enable_dorsal && ooo_cpu[packet->cpu].rob_pos_is_dorsal(packet->rob_position)))
        {
            stats.pseudo_perfect.data_load_miss_eligible_for_pseudo_hit_promotion++;
            
            // promote to hit with a given probability
            if((*dist)(generator))
            {
                stats.pseudo_perfect.data_load_miss_promoted_pseudo_hit++;
                match_way = 0;
            }
        }
    }

    // pseudo-perfect L2C for missing frontal/dorsal loads
    if(packet->is_data && packet->type == LOAD    // this should only be modeled for data loads
        && match_way == -1                        // that are missing in the actual cache
        && (cache_type == IS_L2C && knob::l2c_pseudo_perfect_enable))
    {
        stats.pseudo_perfect.data_load_misses++;

        if((knob::l2c_pseudo_perfect_enable_frontal && ooo_cpu[packet->cpu].rob_pos_is_frontal(packet->rob_position))
            || (knob::l2c_pseudo_perfect_enable_dorsal && ooo_cpu[packet->cpu].rob_pos_is_dorsal(packet->rob_position)))
        {
            stats.pseudo_perfect.data_load_miss_eligible_for_pseudo_hit_promotion++;
            
            // promote to hit with a given probability
            if((*dist)(generator))
            {
                stats.pseudo_perfect.data_load_miss_promoted_pseudo_hit++;
                match_way = 0;
            }
        }
    }

    return match_way;
}

bool CACHE::free_lookup(PACKET *packet)
{
    bool hit = false;
    // 1. Check hit in cache block array
    uint32_t set = get_set(packet->address);
    for (uint32_t way = 0; way < NUM_WAY; way++) {
        if (block[set][way].valid && (block[set][way].tag == packet->address)) {
            hit = true;
            // cout << "$, addr, " << hex2str(packet->address) << '\n';
            break;
        }
    }

    // Apply shadow page table page fault override (similar to handle_read)
    if (hit && knob::enable_ptw && packet->type == TRANSLATION && packet->from_ptw) {
        uint64_t shadow_val;
        bool is_pf = false, is_fa;
        if (buddy_allocator.shadow_get_entry(packet->full_addr, (uint8_t)packet->ptw_level, shadow_val, is_pf, is_fa) && is_pf) {
            hit = false; // forced miss due to page fault
        }
    }

    // 2. Check WQ (Write Queue)
    if (!hit) {
        if (WQ.check_queue(packet) != -1) {
            // cout << "WQ, addr, " << hex2str(packet->address) << '\n';
            hit = true;
        }
    }

    // 3. Check RQ (Read Queue)
    if (!hit && RQ) {
        if (RQ->check_queue(packet) != -1) {
            // cout << "RQ, addr, " << hex2str(packet->address) << '\n';
            hit = true;
        }
    }

    // 4. Check MSHR (Miss Status Holding Register)
    if (!hit) {
        if (check_mshr(packet) != -1) {
            // cout << "MSHR, addr, " << hex2str(packet->address) << '\n';
            hit = true;
        }
    }

    // 5. Check PQ (Prefetch Queue)
    if (!hit) {
        if (PQ.check_queue(packet) != -1) {
            hit = true;
        }
    }

    return hit;
}

// =====================================================================
// CACHE INVALIDATION & REQUEST QUEUE MANAGEMENT
// =====================================================================

int CACHE::invalidate_entry(uint64_t inval_addr)
{
    uint32_t set = get_set(inval_addr);
    int match_way = -1;

    if (NUM_SET < set) {
        cerr << "[" << NAME << "_ERROR] " << __func__ << " invalid set index: " << set << " NUM_SET: " << NUM_SET;
        cerr << " inval_addr: " << hex << inval_addr << dec << endl;
        assert(0 && "Set index exceeds NUM_SET in invalidate operation");
    }

    // invalidate
    for (uint32_t way=0; way<NUM_WAY; way++) {
        if (block[set][way].valid && (block[set][way].tag == inval_addr)) {

            block[set][way].valid = 0;

            match_way = way;

            DP ( if (warmup_complete[cpu]) {
            cout << "[" << NAME << "] " << __func__ << " inval_addr: " << hex << inval_addr;  
            cout << " tag: " << block[set][way].tag << " data: " << block[set][way].data << dec;
            cout << " set: " << set << " way: " << way << " lru: " << block[set][way].lru << " cycle: " << current_core_cycle[cpu] << endl; });

            break;
        }
    }

    return match_way;
}

int CACHE::add_rq(PACKET *packet)
{
    // check for the latest writebacks in the write queue
    int wq_index = WQ.check_queue(packet);
    if (wq_index != -1) 
    {
        // l.log(NAME, "WQ-HIT", hex2str(packet->address), hex2str(packet->full_addr), packet->ptw_level, "fetch=", packet->fetch_packet, '\n');
        packet->hit_where = assign_hit_where(cache_type, 2); // hit in WQ
        uint64_t cur_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[packet->cpu];
        add_history_event(cur_cycle, packet->instr_id, packet->virt_addr, packet->address, packet->full_addr, packet->type, "RQ_WQ_HIT", NAME.c_str(), true, false, false, false, 0, packet->hit_where);

        // check fill level
        if (packet->fill_level < fill_level) 
        {
            packet->data = WQ.entry[wq_index].data;

	        if(fill_level == FILL_L2)
	        {
		        if(packet->fill_l1i)
		        {
		            upper_level_icache[packet->cpu]->return_data(packet);
		        }
		        if(packet->fill_l1d)
		        {
		            upper_level_dcache[packet->cpu]->return_data(packet);
		        }
	        }
	        else
	        {
		        if (packet->instruction)
                {
		            upper_level_icache[packet->cpu]->return_data(packet);
                }
		        if (packet->is_data)
                {
		            upper_level_dcache[packet->cpu]->return_data(packet);
                }
	        }
        }

#ifdef SANITY_CHECK
        if (cache_type == IS_ITLB) {
            cerr << "[" << NAME << "] Unexpected WQ hit in ITLB. Packet addr: 0x" << hex << packet->address << dec << " instr_id: " << packet->instr_id << endl;
            assert(0 && "ITLB WQ hit unexpected");
        }
        else if (cache_type == IS_DTLB) {
            cerr << "[" << NAME << "] Unexpected WQ hit in DTLB. Packet addr: 0x" << hex << packet->address << dec << " instr_id: " << packet->instr_id << endl;
            assert(0 && "DTLB WQ hit unexpected");
        }
        else if (cache_type == IS_L1I) {
            cerr << "[" << NAME << "] Unexpected WQ hit in L1I. Packet addr: 0x" << hex << packet->address << dec << " instr_id: " << packet->instr_id << endl;
            assert(0 && "L1I WQ hit unexpected");
        }
#endif
        // update processed packets
        if ((cache_type == IS_L1D) && (packet->type != PREFETCH)) 
        {
            if (PROCESSED.occupancy < PROCESSED.SIZE)
                PROCESSED.add_queue(packet, current_core_cycle[packet->cpu]);

            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_RQ] " << __func__ << " instr_id: " << packet->instr_id << " found recent writebacks";
            cout << hex << " read: " << packet->address << " writeback: " << WQ.entry[wq_index].address << dec;
            cout << " index: " << MAX_READ << " rob_signal: " << packet->rob_signal << endl; });
        }

        HIT[packet->type]++;
        ACCESS[packet->type]++;

        service_time_hist.update(0);

        WQ.FORWARD++;
        RQ->ACCESS++;

        return -1;
    }

    // check for duplicates in the read queue
    int index = RQ->check_queue(packet);

    if (index != -1) 
    {
        if(packet->from_ptw)
        cout << "Merge, RQ, " << NAME << ", addr, " << hex2str(packet->address) << ", instrid, " << packet->instr_id << '\n';
        // l.log(NAME, "RQ-HIT", hex2str(packet->address), hex2str(packet->full_addr), packet->ptw_level, '\n');
        packet->hit_where = assign_hit_where(cache_type, 1); // hit in RQ
        PACKET& rq_entry = RQ->get_entry(index);
        uint64_t cur_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[packet->cpu];
        add_history_event(cur_cycle, packet->instr_id, packet->virt_addr, packet->address, packet->full_addr, packet->type, "RQ_MERGE", NAME.c_str(), false, false, false, true, rq_entry.instr_id, packet->hit_where);

        if (packet->instruction) 
        {
            uint32_t rob_index = packet->rob_index;
            rq_entry.rob_index_depend_on_me.insert(rob_index);
            rq_entry.instruction = 1; // add as instruction type
            rq_entry.instr_merged = 1;

            DP (if (warmup_complete[packet->cpu]) {
            cout << "[INSTR_MERGED] " << __func__ << " cpu: " << packet->cpu << " instr_id: " << rq_entry.instr_id;
            cout << " merged rob_index: " << rob_index << " instr_id: " << packet->instr_id << endl; });
        }
        else 
        {
            // mark merged consumer
            if (packet->type == RFO) 
            {
                uint32_t sq_index = packet->sq_index;
                rq_entry.sq_index_depend_on_me.insert(sq_index);
                rq_entry.store_merged = 1;
                
                DP (if (warmup_complete[packet->cpu]) {
                cout << "[RFO merging in RQ] cache_type " << cache_type
                << " incoming_type " << packet->type << " exsisting_type " << rq_entry.type
                << " incoming_ROB_index " << packet->rob_index << " exsisting_ROB_index " << rq_entry.rob_index
                << " incoming_ROB_pos " << packet->rob_position << " exsisting_ROB_pos " << rq_entry.rob_position << endl; });
            }
            else 
            {
                uint32_t lq_index = packet->lq_index; 
                rq_entry.lq_index_depend_on_me.insert(lq_index);
                rq_entry.load_merged = 1;

                DP (if (warmup_complete[packet->cpu]) {
                cout << "[LOAD merging in RQ] cache_type " << cache_type
                << " incoming_type " << packet->type << " exsisting_type " << rq_entry.type
                << " incoming_ROB_index " << packet->rob_index << " exsisting_ROB_index " << rq_entry.rob_index
                << " incoming_ROB_pos " << packet->rob_position << " exsisting_ROB_pos " << rq_entry.rob_position << endl; });

                DP (if (warmup_complete[packet->cpu]) {
                cout << "[DATA_MERGED] " << __func__ << " cpu: " << packet->cpu << " instr_id: " << rq_entry.instr_id;
                cout << " merged rob_index: " << packet->rob_index << " instr_id: " << packet->instr_id << " lq_index: " << packet->lq_index << endl; });
            }
            rq_entry.is_data = 1; // add as data type
        }

	    if((packet->fill_l1i) && (rq_entry.fill_l1i != 1))
	    {
	        rq_entry.fill_l1i = 1;
	    }
	    if((packet->fill_l1d) && (rq_entry.fill_l1d != 1))
	    {
	        rq_entry.fill_l1d = 1;
	    }

        RQ->MERGED++;
        RQ->ACCESS++;

        return index; // merged index
    }

    // check occupancy
    if (RQ->occupancy == RQ_SIZE) 
    {
        RQ->FULL++;
        return -2; // cannot handle this request
    }

    // if there is no duplicate, add it to RQ
    uint64_t enque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[packet->cpu];
    index = RQ->add_queue(packet, enque_cycle);
    PACKET& rq_entry = RQ->get_entry(index);
    add_history_event(enque_cycle, packet->instr_id, packet->virt_addr, packet->address, packet->full_addr, packet->type, "ADD_RQ", NAME.c_str(), false, false, false, false, 0, packet->hit_where);

    // ADD LATENCY
    rq_entry.event_cycle = enque_cycle;
    rq_entry.started_latency = 0;

    // // Deadblock pred: predict read hit/miss for LOADs at LLC
    // if (cache_type == IS_LLC && rq_entry.type == LOAD && knob::knob_doa_predictor && llc_pred_perc != NULL)
    // {
    //     bool pred_hit = llc_pred_perc->predict(rq_entry.full_addr, rq_entry.ip);
    //     rq_entry.doa_pred_bit = pred_hit;
    // }

    DP ( if (warmup_complete[rq_entry.cpu]) {
    cout << "[" << NAME << "_RQ] " <<  __func__ << " instr_id: " << rq_entry.instr_id << " address: " << hex << rq_entry.address;
    cout << " full_addr: " << rq_entry.full_addr << dec;
    cout << " type: " << +rq_entry.type << " head: " << RQ->get_head() << " tail: " << RQ->get_tail() << " occupancy: " << RQ->occupancy;
    cout << " event: " << rq_entry.event_cycle << " current: " << current_core_cycle[rq_entry.cpu] << endl; });

    if (packet->address == 0) {
        cerr << "[" << NAME << "_ERROR] RQ packet with zero address: instr_id=" << packet->instr_id;
        cerr << " type=" << +packet->type << " cpu=" << packet->cpu << endl;
        assert(0 && "RQ packet address is zero");
    }

    RQ->TO_CACHE++;
    RQ->ACCESS++;

    return -1;
}

int CACHE::add_wq(PACKET *packet)
{
    // check for duplicates in the write queue
    int index = WQ.check_queue(packet);
    if (index != -1) 
    {
        uint64_t cur_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[packet->cpu];
        add_history_event(cur_cycle, packet->instr_id, packet->virt_addr, packet->address, packet->full_addr, packet->type, "WQ_MERGE", NAME.c_str(), false, false, false, true, WQ.entry[index].instr_id, packet->hit_where);

        WQ.MERGED++;
        WQ.ACCESS++;

        return index; // merged index
    }

    // sanity check
    if (WQ.occupancy >= WQ.SIZE) {
        cerr << "[" << NAME << "_ERROR] WQ overflow: occupancy=" << WQ.occupancy << " SIZE=" << WQ.SIZE;
        cerr << " addr=0x" << hex << packet->address << " full_addr=0x" << packet->full_addr;
        cerr << " instr_id=" << dec << packet->instr_id << " type=" << +packet->type << endl;
        assert(0 && "Write queue is full");
    }

    // if there is no duplicate, add it to the write queue
    uint64_t enque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[packet->cpu];
    index = WQ.add_queue(packet, enque_cycle);
    add_history_event(enque_cycle, packet->instr_id, packet->virt_addr, packet->address, packet->full_addr, packet->type, "ADD_WQ", NAME.c_str(), false, false, false, false, 0, packet->hit_where);
    // index = WQ.tail;
    // if (WQ.entry[index].address != 0) 
    // {
    //     cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
    //     cerr << " address: " << hex << WQ.entry[index].address;
    //     cerr << " full_addr: " << WQ.entry[index].full_addr << dec << endl;
    //     assert(0);
    // }

    // WQ.entry[index] = *packet;

    // ADD LATENCY
    WQ.entry[index].event_cycle = enque_cycle;
    WQ.entry[index].started_latency = 0;

    // WQ.occupancy++;
    // WQ.tail++;
    // if (WQ.tail >= WQ.SIZE)
    //     WQ.tail = 0;

    DP (if (warmup_complete[WQ.entry[index].cpu]) {
    cout << "[" << NAME << "_WQ] " <<  __func__ << " instr_id: " << WQ.entry[index].instr_id << " address: " << hex << WQ.entry[index].address;
    cout << " full_addr: " << WQ.entry[index].full_addr << dec;
    cout << " head: " << WQ.head << " tail: " << WQ.tail << " occupancy: " << WQ.occupancy;
    cout << " data: " << hex << WQ.entry[index].data << dec;
    cout << " event: " << WQ.entry[index].event_cycle << " current: " << current_core_cycle[WQ.entry[index].cpu] << endl; });

    WQ.TO_CACHE++;
    WQ.ACCESS++;

    return -1;
}

int CACHE::prefetch_line(uint64_t ip, uint64_t base_addr, uint64_t pf_addr, int pf_fill_level, uint32_t prefetch_metadata)
{
    pf_requested++;

    if (PQ.occupancy < PQ.SIZE) 
    {
        if ((base_addr>>LOG2_PAGE_SIZE) == (pf_addr>>LOG2_PAGE_SIZE)) 
        {
            PACKET pf_packet;
            pf_packet.fill_level = pf_fill_level;
	        pf_packet.pf_origin_level = fill_level;
            if(pf_fill_level == FILL_L1)
            {
                pf_packet.fill_l1d = 1;
            }
	        pf_packet.pf_metadata = prefetch_metadata;
            pf_packet.cpu = cpu;
            //pf_packet.data_index = LQ.entry[lq_index].data_index;
            //pf_packet.lq_index = lq_index;
            pf_packet.address = pf_addr >> LOG2_BLOCK_SIZE;
            pf_packet.full_addr = pf_addr;
            //pf_packet.instr_id = LQ.entry[lq_index].instr_id;
            //pf_packet.rob_index = LQ.entry[lq_index].rob_index;
            pf_packet.ip = ip;
            pf_packet.type = PREFETCH;
            pf_packet.asid[0] = cpu;
            pf_packet.asid[1] = cpu * knob::num_threads_per_core;
            pf_packet.event_cycle = current_core_cycle[cpu];

            // give a dummy 0 as the IP of a prefetch
            add_pq(&pf_packet);

            pf_issued++;

            return 1;
        }
    }
    else
    {
        pf_dropped++;
    }

    return 0;
}

int CACHE::kpc_prefetch_line(uint64_t base_addr, uint64_t pf_addr, int pf_fill_level, int delta, int depth, int signature, int confidence, uint32_t prefetch_metadata)
{
    if (PQ.occupancy < PQ.SIZE) 
    {
        if ((base_addr>>LOG2_PAGE_SIZE) == (pf_addr>>LOG2_PAGE_SIZE)) 
        {
            PACKET pf_packet;
            pf_packet.fill_level = pf_fill_level;
	        pf_packet.pf_origin_level = fill_level;
            if(pf_fill_level == FILL_L1)
            {
                pf_packet.fill_l1d = 1;
            }
	        pf_packet.pf_metadata = prefetch_metadata;
            pf_packet.cpu = cpu;
            //pf_packet.data_index = LQ.entry[lq_index].data_index;
            //pf_packet.lq_index = lq_index;
            pf_packet.address = pf_addr >> LOG2_BLOCK_SIZE;
            pf_packet.full_addr = pf_addr;
            //pf_packet.instr_id = LQ.entry[lq_index].instr_id;
            //pf_packet.rob_index = LQ.entry[lq_index].rob_index;
            pf_packet.ip = 0;
            pf_packet.type = PREFETCH;
            pf_packet.asid[0] = cpu;
            pf_packet.asid[1] = cpu * knob::num_threads_per_core;
            pf_packet.delta = delta;
            pf_packet.depth = depth;
            pf_packet.signature = signature;
            pf_packet.confidence = confidence;
            pf_packet.event_cycle = current_core_cycle[cpu];

            // give a dummy 0 as the IP of a prefetch
            add_pq(&pf_packet);

            pf_issued++;

            return 1;
        }
    }

    return 0;
}

int CACHE::add_pq(PACKET *packet)
{
    // check for the latest wirtebacks in the write queue
    int wq_index = WQ.check_queue(packet);
    if (wq_index != -1) 
    {
        packet->hit_where = assign_hit_where(cache_type, 2); // prefetch hitting in WQ
        uint64_t cur_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[packet->cpu];
        add_history_event(cur_cycle, packet->instr_id, packet->virt_addr, packet->address, packet->full_addr, packet->type, "PQ_WQ_HIT", NAME.c_str(), true, false, false, false, 0, packet->hit_where);

        // check fill level
        if (packet->fill_level < fill_level) 
        {
            packet->data = WQ.entry[wq_index].data;

	        if(fill_level == FILL_L2)
	        {
                if(packet->fill_l1i)
                {
                    upper_level_icache[packet->cpu]->return_data(packet);
                }
                if(packet->fill_l1d)
		        {
		            upper_level_dcache[packet->cpu]->return_data(packet);
		        }
	        }
	        else
	        {
                if (packet->instruction)
                {
                    upper_level_icache[packet->cpu]->return_data(packet);
                }
                if (packet->is_data)
                {
                    upper_level_dcache[packet->cpu]->return_data(packet);
                }
	        }
        }

        HIT[packet->type]++;
        ACCESS[packet->type]++;

        WQ.FORWARD++;
        PQ.ACCESS++;

        return -1;
    }

    // check for duplicates in the PQ
    int index = PQ.check_queue(packet);
    if (index != -1) 
    {
        uint64_t cur_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[packet->cpu];
        add_history_event(cur_cycle, packet->instr_id, packet->virt_addr, packet->address, packet->full_addr, packet->type, "PQ_MERGE", NAME.c_str(), false, false, false, true, PQ.entry[index].instr_id, packet->hit_where);
        if (packet->fill_level < PQ.entry[index].fill_level)
	    {
            PQ.entry[index].fill_level = packet->fill_level;
	    }
	    if((packet->instruction == 1) && (PQ.entry[index].instruction != 1))
	    {
	        PQ.entry[index].instruction = 1;
	    }
	    if((packet->is_data == 1) && (PQ.entry[index].is_data != 1))
	    {
	        PQ.entry[index].is_data = 1;
	    }
	    if((packet->fill_l1i) && (PQ.entry[index].fill_l1i != 1))
	    {
	        PQ.entry[index].fill_l1i = 1;
	    }
	    if((packet->fill_l1d) && (PQ.entry[index].fill_l1d != 1))
	    {
	        PQ.entry[index].fill_l1d = 1;
	    }

        PQ.MERGED++;
        PQ.ACCESS++;

        return index; // merged index
    }

    // check occupancy
    if (PQ.occupancy == PQ_SIZE) 
    {
        PQ.FULL++;

        DP ( if (warmup_complete[packet->cpu]) {
        cout << "[" << NAME << "] cannot process add_pq since it is full" << endl; });
        return -2; // cannot handle this request
    }

    // if there is no duplicate, add it to PQ
    uint64_t enque_cycle = cache_type == IS_LLC ? uncore.cycle : current_core_cycle[packet->cpu];
    index = PQ.add_queue(packet, enque_cycle);
    add_history_event(enque_cycle, packet->instr_id, packet->virt_addr, packet->address, packet->full_addr, packet->type, "ADD_PQ", NAME.c_str(), false, false, false, false, 0, packet->hit_where);
//     index = PQ.tail;

// #ifdef SANITY_CHECK
//     if (PQ.entry[index].address != 0) {
//         cerr << "[" << NAME << "_ERROR] " << __func__ << " is not empty index: " << index;
//         cerr << " address: " << hex << PQ.entry[index].address;
//         cerr << " full_addr: " << PQ.entry[index].full_addr << dec << endl;
//         assert(0);
//     }
// #endif

//     PQ.entry[index] = *packet;

    // ADD LATENCY
    if (PQ.entry[index].event_cycle < current_core_cycle[packet->cpu])
    {
        PQ.entry[index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    }
    else
    {
        PQ.entry[index].event_cycle += LATENCY;
    }

    // PQ.occupancy++;
    // PQ.tail++;
    // if (PQ.tail >= PQ.SIZE)
    //     PQ.tail = 0;

    DP ( if (warmup_complete[PQ.entry[index].cpu]) {
    cout << "[" << NAME << "_PQ] " <<  __func__ << " instr_id: " << PQ.entry[index].instr_id << " address: " << hex << PQ.entry[index].address;
    cout << " full_addr: " << PQ.entry[index].full_addr << dec;
    cout << " type: " << +PQ.entry[index].type << " head: " << PQ.head << " tail: " << PQ.tail << " occupancy: " << PQ.occupancy;
    cout << " event: " << PQ.entry[index].event_cycle << " current: " << current_core_cycle[PQ.entry[index].cpu] << endl; });

    if (packet->address == 0) {
        cerr << "[" << NAME << "_ERROR] PQ packet with zero address: instr_id=" << packet->instr_id;
        cerr << " type=" << +packet->type << " cpu=" << packet->cpu << endl;
        assert(0 && "PQ packet address is zero");
    }

    PQ.TO_CACHE++;
    PQ.ACCESS++;

    return -1;
}

// =====================================================================
// RESPONSE HANDLING - Complete requests and route responses upward
// =====================================================================
// Pravesh: This section includes PTW page table walker integration
//

void CACHE::return_data(PACKET *packet)
{
    // check MSHR information
    int mshr_index = check_mshr(packet);

    // sanity check
    if (mshr_index == -1) {
        cerr << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << packet->instr_id << " cannot find a matching entry!";
        cerr << " full_addr: " << hex << packet->full_addr;
        cerr << " address: " << packet->address << dec;
        cerr << " event: " << packet->event_cycle << " current: " << current_core_cycle[packet->cpu] << endl;
        
        assert(0 && "MSHR entry not found for returning packet");
    }

    // MSHR holds the most updated information about this request
    // no need to do memcpy
    MSHR.num_returned++;
    MSHR.entry[mshr_index].returned = COMPLETED;
    MSHR.entry[mshr_index].data = packet->data;
    MSHR.entry[mshr_index].pf_metadata = packet->pf_metadata;

    // pravesh
    MSHR.entry[mshr_index].hit_where = packet->hit_where;
    MSHR.entry[mshr_index].pwc_miss_mem_hitwhere = packet->pwc_miss_mem_hitwhere;
    MSHR.entry[mshr_index].went_offchip = packet->went_offchip;

    // ADD LATENCY
    if (MSHR.entry[mshr_index].event_cycle < current_core_cycle[packet->cpu])
        MSHR.entry[mshr_index].event_cycle = current_core_cycle[packet->cpu] + LATENCY;
    else
        MSHR.entry[mshr_index].event_cycle += LATENCY;

    update_fill_cycle();

    // if(packet->type == TRANSLATION){
    //     cout <<NAME<< ",return,cpu,"<<packet->cpu<<",instr,"<<packet->instr_id<<std::hex<<",addr,"<<hex2str(packet->address)<<",vaddr,"<<hex2str(packet->virt_addr)<<",d,"<<hex2str(packet->data)<<std::dec<<",lvl,"<<packet->ptw_level<<'\n'; 
    // }

    DP (if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "_MSHR] " <<  __func__ << " instr_id: " << MSHR.entry[mshr_index].instr_id;
    cout << " address: " << hex << MSHR.entry[mshr_index].address << " full_addr: " << MSHR.entry[mshr_index].full_addr;
    cout << " data: " << MSHR.entry[mshr_index].data << dec << " num_returned: " << MSHR.num_returned;
    cout << " index: " << mshr_index << " occupancy: " << MSHR.occupancy;
    cout << " event: " << MSHR.entry[mshr_index].event_cycle << " current: " << current_core_cycle[packet->cpu] << " next: " << MSHR.next_fill_cycle << endl; });
}

void CACHE::update_fill_cycle()
{
    // update next_fill_cycle
    uint64_t min_cycle = UINT64_MAX;
    uint32_t min_index = MSHR.SIZE;
    for (uint32_t i=0; i<MSHR.SIZE; i++) {
        if ((MSHR.entry[i].returned == COMPLETED) && (MSHR.entry[i].event_cycle < min_cycle)) {
            min_cycle = MSHR.entry[i].event_cycle;
            min_index = i;
        }

        DP (if (warmup_complete[MSHR.entry[i].cpu]) {
        cout << "[" << NAME << "_MSHR] " <<  __func__ << " checking instr_id: " << MSHR.entry[i].instr_id;
        cout << " address: " << hex << MSHR.entry[i].address << " full_addr: " << MSHR.entry[i].full_addr;
        cout << " data: " << MSHR.entry[i].data << dec << " returned: " << +MSHR.entry[i].returned << " fill_level: " << MSHR.entry[i].fill_level;
        cout << " index: " << i << " occupancy: " << MSHR.occupancy;
        cout << " event: " << MSHR.entry[i].event_cycle << " current: " << current_core_cycle[MSHR.entry[i].cpu] << " next: " << MSHR.next_fill_cycle << endl; });
    }
    
    MSHR.next_fill_cycle = min_cycle;
    MSHR.next_fill_index = min_index;
    if (min_index < MSHR.SIZE) {

        DP (if (warmup_complete[MSHR.entry[min_index].cpu]) {
        cout << "[" << NAME << "_MSHR] " <<  __func__ << " instr_id: " << MSHR.entry[min_index].instr_id;
        cout << " address: " << hex << MSHR.entry[min_index].address << " full_addr: " << MSHR.entry[min_index].full_addr;
        cout << " data: " << MSHR.entry[min_index].data << dec << " num_returned: " << MSHR.num_returned;
        cout << " event: " << MSHR.entry[min_index].event_cycle << " current: " << current_core_cycle[MSHR.entry[min_index].cpu] << " next: " << MSHR.next_fill_cycle << endl; });
    }
}

// =====================================================================
// MSHR MANAGEMENT - Miss Status Holding Register operations
// =====================================================================

int CACHE::check_mshr(PACKET *packet)
{
    // search mshr
  //bool instruction_and_data_collision = false;
  
    for (uint32_t index=0; index<MSHR_SIZE; index++)
    {
        if (MSHR.entry[index].address == packet->address)
	    {
            //if(MSHR.entry[index].instruction != packet->instruction)
            //  {
            //    instruction_and_data_collision = true;
            //  }
            //else
            //  {
            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_MSHR] " << __func__ << " same entry instr_id: " << packet->instr_id << " prior_id: " << MSHR.entry[index].instr_id;
            cout << " address: " << hex << packet->address;
            cout << " full_addr: " << packet->full_addr << dec << endl; });
	    
	        return index;
	        //  }
	    }
    }

    //if(instruction_and_data_collision) // remove instruction-and-data collision safeguard
    //  {
	//return -2;
    //  }

    DP ( if (warmup_complete[packet->cpu]) {
    cout << "[" << NAME << "_MSHR] " << __func__ << " new address: " << hex << packet->address;
    cout << " full_addr: " << packet->full_addr << dec << endl; });

    DP ( if (warmup_complete[packet->cpu] && (MSHR.occupancy == MSHR_SIZE)) { 
    cout << "[" << NAME << "_MSHR] " << __func__ << " mshr is full";
    cout << " instr_id: " << packet->instr_id << " mshr occupancy: " << MSHR.occupancy;
    cout << " address: " << hex << packet->address;
    cout << " full_addr: " << packet->full_addr << dec;
    cout << " cycle: " << current_core_cycle[packet->cpu] << endl; });

    return -1;
}

void CACHE::add_mshr(PACKET *packet)
{
    uint32_t index = 0;

    packet->cycle_enqueued = current_core_cycle[packet->cpu];
    add_history_event(packet->cycle_enqueued, packet->instr_id, packet->virt_addr, packet->address, packet->full_addr, packet->type, "ADD_MSHR", NAME.c_str(), false, false, false, false, 0, packet->hit_where);

    // search mshr
    for (index=0; index<MSHR_SIZE; index++) 
    {
        if (MSHR.entry[index].address == 0) 
        {
            MSHR.entry[index] = *packet;
            MSHR.entry[index].returned = INFLIGHT;
            MSHR.occupancy++;

            DP ( if (warmup_complete[packet->cpu]) {
            cout << "[" << NAME << "_MSHR] " << __func__ << " instr_id: " << packet->instr_id;
            cout << " address: " << hex << packet->address << " full_addr: " << packet->full_addr << dec;
            cout << " index: " << index << " occupancy: " << MSHR.occupancy << endl; });

            break;
        }
    }
}

uint32_t CACHE::get_occupancy(uint8_t queue_type, uint64_t address)
{
    if (queue_type == 0)
        return MSHR.occupancy;
    else if (queue_type == 1)
        return RQ->occupancy;
    else if (queue_type == 2)
        return WQ.occupancy;
    else if (queue_type == 3)
        return PQ.occupancy;

    return 0;
}

uint32_t CACHE::get_size(uint8_t queue_type, uint64_t address)
{
    if (queue_type == 0)
        return MSHR.SIZE;
    else if (queue_type == 1)
        return RQ->SIZE;
    else if (queue_type == 2)
        return WQ.SIZE;
    else if (queue_type == 3)
        return PQ.SIZE;

    return 0;
}

void CACHE::increment_WQ_FULL(uint64_t address)
{
    WQ.FULL++;
}

void CACHE::prefetcher_feedback(uint64_t &pref_gen, uint64_t &pref_fill, uint64_t &pref_used, uint64_t &pref_late)
{
    pref_gen = pf_issued;
    pref_fill = pf_filled;
    pref_used = pf_useful;
    pref_late = pf_late;
}

void CACHE::track_max_lru_on_hit(uint32_t set, uint32_t way)
{
    bool track_max_lru = false;
    uint8_t pkt_type = block[set][way].footprint.packet_type;
    if (knob::max_lru_before_eviction_block_type == "ALL") {
        track_max_lru = true;
    } else if (knob::max_lru_before_eviction_block_type == "LOAD" && pkt_type == LOAD) {
        track_max_lru = true;
    } else if (knob::max_lru_before_eviction_block_type == "PREFETCH" && pkt_type == PREFETCH) {
        track_max_lru = true;
    } else if (knob::max_lru_before_eviction_block_type == "TRANSLATION" && pkt_type == TRANSLATION) {
        track_max_lru = true;
    } else if (knob::max_lru_before_eviction_block_type == "WRITEBACK" && pkt_type == WRITEBACK) {
        track_max_lru = true;
    }

    if (track_max_lru) {
        if (block[set][way].lru > block[set][way].max_lru_before_eviction) {
            block[set][way].max_lru_before_eviction = block[set][way].lru;
        }
    }
}

void CACHE::track_stats_from_victim(uint32_t set, uint32_t way)
{
    stats.eviction.total++;

    // Footprint tracking stats at eviction
    if (block[set][way].footprint.track_footprint) {
        uint32_t footprint_size = 0;
        for (int i = 0; i < 8; i++) {
            if ((block[set][way].footprint.footprint & (1 << i)) != 0) {
                footprint_size++;
            }
        }
        if (block[set][way].footprint.footprint_changed) {
            uint32_t max_lru = block[set][way].footprint.max_lru_footprint_change;
            if (max_lru < 64) {
                stats.footprint.general.max_footprint_lru_hist[max_lru][footprint_size]++;
            }
            
            // Translation stats (together and level-specific)
            if (block[set][way].footprint.packet_type == TRANSLATION) {
                if (max_lru < 64) {
                    stats.footprint.trans_together.max_footprint_lru_hist[max_lru][footprint_size]++;
                }
                uint32_t lvl = block[set][way].footprint.ptw_level;
                if (lvl < 5) {
                    if (max_lru < 64) {
                        stats.footprint.trans_level[lvl].max_footprint_lru_hist[max_lru][footprint_size]++;
                    }
                }
            }
        }
    }

    bool track_max_lru = false;
    uint8_t pkt_type = block[set][way].footprint.packet_type;
    if (knob::max_lru_before_eviction_block_type == "ALL") {
        track_max_lru = true;
    } else if (knob::max_lru_before_eviction_block_type == "LOAD" && pkt_type == LOAD) {
        track_max_lru = true;
    } else if (knob::max_lru_before_eviction_block_type == "PREFETCH" && pkt_type == PREFETCH) {
        track_max_lru = true;
    } else if (knob::max_lru_before_eviction_block_type == "TRANSLATION" && pkt_type == TRANSLATION) {
        track_max_lru = true;
    } else if (knob::max_lru_before_eviction_block_type == "WRITEBACK" && pkt_type == WRITEBACK) {
        track_max_lru = true;
    }

    if (track_max_lru) {
        uint32_t max_lru = block[set][way].max_lru_before_eviction;
        if (max_lru < 128) {
            stats.max_lru_before_eviction_hist[max_lru]++;
        }
    }

    // Invalid PTE tracking for translation blocks
    if (block[set][way].footprint.packet_type == TRANSLATION) {
        uint32_t invalid_count = 0;
        for (uint32_t i = 0; i < 8; i++) {
            uint64_t pte_addr = (block[set][way].address << 6) + i * 8;
            uint64_t val = 0;
            bool is_page_fault = false;
            bool is_first_access = false;
            bool tracked = buddy_allocator.shadow_get_entry(pte_addr, (uint8_t)block[set][way].footprint.ptw_level, val, is_page_fault, is_first_access);
            if (!tracked || is_page_fault) {
                invalid_count++;
            }
        }
        if (invalid_count <= 8) {
            stats.footprint.translation_invalid_pte_count_hist[invalid_count]++;
            uint32_t lvl = block[set][way].footprint.ptw_level;
            if (lvl < 5) {
                stats.footprint.trans_level_invalid_pte_count_hist[lvl][invalid_count]++;
            }
        }
    }

    // Entry reuse tracking
    if (block[set][way].footprint.track_footprint) {
        for (uint32_t e = 0; e < 8; e++) {
            uint32_t c = block[set][way].footprint.entry_reuse[e];
            int b = -1;
            if (c == 1) {
                b = 0;
            } else if (c >= 2 && c <= 4) {
                b = 1;
            } else if (c >= 5 && c <= 8) {
                b = 2;
            } else if (c >= 9 && c <= 16) {
                b = 3;
            } else if (c > 16) {
                b = 4;
            }
            
            if (b >= 0) {
                stats.footprint.general_entry_reuse_hist[e][b]++;
                if (block[set][way].footprint.packet_type == TRANSLATION) {
                    stats.footprint.trans_together_entry_reuse_hist[e][b]++;
                    uint32_t lvl = block[set][way].footprint.ptw_level;
                    if (lvl < 5) {
                        stats.footprint.trans_level_entry_reuse_hist[lvl][e][b]++;
                    }
                }
            }
        }
    }

    // keep count of blocks that have seen at least one reuse of specific access type
    for(uint32_t type = LOAD; type < NUM_TYPES; ++type)
    {
        if(block[set][way].reuse[type] > 0) 
        {
            stats.eviction.atleast_one_reuse++;
            stats.eviction.atleast_one_reuse_cat[type]++;
        }
    }

    // track total reuse
    uint32_t reuse = 0;
    for(uint32_t index = 0; index < NUM_TYPES; ++index) reuse += block[set][way].reuse[index];
    stats.eviction.all_reuse_total += reuse;
    if(reuse >= stats.eviction.all_reuse_max) stats.eviction.all_reuse_max = reuse;
    if(reuse <= stats.eviction.all_reuse_min) stats.eviction.all_reuse_min = reuse;

    // track reuse of individual access type
    for(uint32_t type = LOAD; type < NUM_TYPES; ++type)
    {
        reuse = block[set][way].reuse[type];
        stats.eviction.cat_reuse_total[type] += reuse;
        if(reuse >= stats.eviction.cat_reuse_max[type]) stats.eviction.cat_reuse_max[type] = reuse;
        if(reuse <= stats.eviction.cat_reuse_min[type]) stats.eviction.cat_reuse_min[type] = reuse;
    }

    // track cacheblocks that are reused by only frontal/dorsal loads
    if(block[set][way].reuse_frontal_dorsal[FRONTAL] > 0 
        && block[set][way].reuse_frontal_dorsal[DORSAL] == 0 
        && block[set][way].reuse_frontal_dorsal[NONE] == 0)
    {
        stats.eviction.reuse_only_frontal++;
    }
    else if(block[set][way].reuse_frontal_dorsal[DORSAL] > 0 
        && block[set][way].reuse_frontal_dorsal[FRONTAL] == 0 
        && block[set][way].reuse_frontal_dorsal[NONE] == 0)
    {
        stats.eviction.reuse_only_dorsal++;
    }
    else if(block[set][way].reuse_frontal_dorsal[NONE] > 0 
        && block[set][way].reuse_frontal_dorsal[FRONTAL] == 0 
        && block[set][way].reuse_frontal_dorsal[DORSAL] == 0)
    {
        stats.eviction.reuse_only_none++;
    }
    else if(block[set][way].reuse[LOAD] > 0)
    {
        stats.eviction.reuse_mixed++; // this still counts instruction load reuses
    }

    // track number of dependents for only those blocks that have seen at least one load reuse
    if(knob::track_load_hit_dependency_in_cache && block[set][way].reuse[LOAD] > 0)
    {
        // total dependents
        uint32_t dep_all = block[set][way].dependents;
        stats.eviction.dep_all_total += dep_all;
        if(dep_all >= stats.eviction.dep_all_max) stats.eviction.dep_all_max = dep_all;
        if(dep_all <= stats.eviction.dep_all_min) stats.eviction.dep_all_min = dep_all;
        if(dependent_map.find(dep_all) == dependent_map.end())
            dependent_map.insert(std::pair<uint64_t, uint64_t>(dep_all, 1));
        else
            dependent_map[dep_all]++;

        // mispredicted branches in the dependency chain
        uint32_t dep_branch_mispred = block[set][way].cat_dependents[DEP_INSTR_BRANCH_MISPRED];
        stats.eviction.dep_branch_mispred_total += dep_branch_mispred;
        if(dep_branch_mispred >= stats.eviction.dep_branch_mispred_max) stats.eviction.dep_branch_mispred_max = dep_branch_mispred;
        if(dep_branch_mispred <= stats.eviction.dep_branch_mispred_min) stats.eviction.dep_branch_mispred_min = dep_branch_mispred;

        // all branches (correct+mispred) in the dependency chain
        uint32_t dep_branch = block[set][way].cat_dependents[DEP_INSTR_BRANCH_MISPRED] + block[set][way].cat_dependents[DEP_INSTR_BRANCH_CORRECT];
        stats.eviction.dep_branch_total += dep_branch;
        if(dep_branch >= stats.eviction.dep_branch_max) stats.eviction.dep_branch_max = dep_branch;
        if(dep_branch <= stats.eviction.dep_branch_min) stats.eviction.dep_branch_min = dep_branch;

        // loads in the dependency chain
        uint32_t dep_load = block[set][way].cat_dependents[DEP_INSTR_LOAD];
        stats.eviction.dep_load_total += dep_load;
        if(dep_load >= stats.eviction.dep_load_max) stats.eviction.dep_load_max = dep_load;
        if(dep_load <= stats.eviction.dep_load_min) stats.eviction.dep_load_min = dep_load;
    }

    // Deadblock pred: record eviction outcome (doa_pred_bit vs actual usage)
    if (cache_type == IS_LLC && knob::knob_doa_predictor && llc_pred_perc != NULL)
    {
        bool pred_doa = block[set][way].doa_pred_bit;
        bool actual_doa = (block[set][way].usage == 0);
        llc_pred_perc->record_eviction(pred_doa, actual_doa);
        llc_pred_perc->train(block[set][way].full_addr, block[set][way].ip, pred_doa, actual_doa);
    }
}

/* RBERA: this function is not fully complete.
 * Meaning, this does not faithfully assign hit_where for all cases.
 * PLEASE DO NOT USE THIS AS IS.
 */
hit_where_t CACHE::assign_hit_where(uint8_t cache_type, uint32_t where_in_cache)
{
    // where_in_cache:
    // 0 = hit in cache data array
    // 1 = hit in read queue (possible for L1D/L1I. Basically caches that are core-faced)
    // 2 = hit in WQ (possible for L1D/L1I).
    // 3 = hit in MSHR

    if(cache_type == IS_ITLB)
    {
        // // TODO: can ITLB have RQ/WQ hits too?
        // if(where_in_cache == 0)         return hit_where_t::ITLB;
        // else if(where_in_cache == 3)    return hit_where_t::ITLB_MSHR;
        return hit_where_t::ITLB;
    }
    else if(cache_type == IS_DTLB)
    {
        // // TODO: can DTLB have RQ/WQ hits too?
        // if(where_in_cache == 0)         return hit_where_t::DTLB;
        // else if(where_in_cache == 3)    return hit_where_t::DTLB_MSHR;
        return hit_where_t::DTLB;
    }
    else if(cache_type == IS_STLB)
    {
        return hit_where_t::STLB;
    }
    else if(cache_type == IS_L1I)
    {
        if(where_in_cache == 0)         return hit_where_t::L1I;
        else if(where_in_cache == 1)    return hit_where_t::L1I_RQ;
        else if(where_in_cache == 2)    return hit_where_t::L1I_WQ;
        else if(where_in_cache == 3)    return hit_where_t::L1I_MSHR;
    }
    else if(cache_type == IS_L1D)
    {
        if(where_in_cache == 0)         return hit_where_t::L1D;
        else if(where_in_cache == 1)    return hit_where_t::L1D_RQ;
        else if(where_in_cache == 2)    return hit_where_t::L1D_WQ;
        else if(where_in_cache == 3)    return hit_where_t::L1D_MSHR;
    }
    else if(cache_type == IS_L2C)
    {
        if(where_in_cache == 0)         return hit_where_t::L2C;
        else if(where_in_cache == 1)    return hit_where_t::L2C_RQ;
        else if(where_in_cache == 2)    return hit_where_t::L2C_WQ;
        else if(where_in_cache == 3)    return hit_where_t::L2C_MSHR;
    }
    else if(cache_type == IS_LLC)
    {
        if(where_in_cache == 0)         return hit_where_t::LLC;
        else if(where_in_cache == 1)    return hit_where_t::LLC_RQ;
        else if(where_in_cache == 2)    return hit_where_t::LLC_WQ;
        else if(where_in_cache == 3)    return hit_where_t::LLC_MSHR;
    }
    else if(cache_type == IS_DRAM)
    {
       return hit_where_t::DRAM;
    }

    return hit_where_t::INV;
}

void CACHE::record_offchip_event(uint32_t cpu, PACKET* packet)
{
    uint32_t lq_index = packet->lq_index;
    if(packet->type == LOAD)
    {
        if (packet->rob_index >= 0 && (uint32_t)packet->rob_index < ooo_cpu[cpu].ROB.SIZE)
        {
            ooo_cpu[cpu].ROB.entry[packet->rob_index].data_went_offchip = 1; // mark in ROB as well
        }
    }
    else if(packet->type == TRANSLATION)
    {
        if (packet->rob_index >= 0 && (uint32_t)packet->rob_index < ooo_cpu[cpu].ROB.SIZE)
        {
            ooo_cpu[cpu].ROB.entry[packet->rob_index].translation_went_offchip = 1; // mark in ROB as well
        }

        packet->went_offchip = 1; // also mark in the packet for prefetcher feedback
    }
}

void CACHE::send_signal_to_core(uint32_t cpu, PACKET packet)
{
    uint32_t lq_index = packet.lq_index;
    uint32_t rob_index = packet.rob_index;

    if (lq_index >= LQ_SIZE || ooo_cpu[cpu].LQ.entry[lq_index].rob_index != rob_index)
    {
        return;
    }

    ooo_cpu[cpu].LQ.entry[lq_index].went_offchip = 1; // mark in LQ

    // send signal to all merged loads too
    if (knob::offchip_pred_mark_merged_load)
    {
        ITERATE_SET(merged, packet.lq_index_depend_on_me, ooo_cpu[cpu].LQ.SIZE)
        {
            ooo_cpu[cpu].LQ.entry[merged].went_offchip = 1;
        }
    }
}

void CACHE::broadcast_bw(uint8_t bw_level)
{
    /* boradcast to all the attached prefetchers */
    switch(cache_type)
    {
        case IS_L1I:
            break;
        case IS_L1D:
            l1d_prefetcher_broadcast_bw(bw_level);
            break;
        case IS_L2C:
            l2c_prefetcher_broadcast_bw(bw_level);
            break;
        case IS_LLC:
            llc_prefetcher_broadcast_bw(bw_level);
            break;
    }

    /* recursively broadcast to higher caches */
    CACHE *cache = NULL;
    for(uint32_t core = 0; core < NUM_CPUS; ++core)
    {
        if(upper_level_dcache[core])
        {
            cache = (CACHE*)upper_level_dcache[core];
            cache->broadcast_bw(bw_level);
        }
        if(upper_level_icache[core] && upper_level_icache[core] != upper_level_dcache[core])
        {
            cache = (CACHE*)upper_level_icache[core];
            cache->broadcast_bw(bw_level);
        }
    }
}

void CACHE::broadcast_ipc(uint8_t ipc)
{
    if (cache_type == IS_L1D)
        l1d_prefetcher_broadcast_ipc(ipc);
    else if (cache_type == IS_L2C)
        l2c_prefetcher_broadcast_ipc(ipc);
    else if (cache_type == IS_LLC)
        llc_prefetcher_broadcast_ipc(ipc);
}

bool CACHE::search_and_add(uint64_t page)
{
    bool found = false;
    auto it = find_if(page_buffer.begin(), page_buffer.end(), [page](uint64_t p){return p == page;});
    if(it != page_buffer.end()) found = true;
    if(!found)
    {
        if(page_buffer.size() >= knob::semi_perfect_cache_page_buffer_size)
        {
            page_buffer.pop_front();
        }
        page_buffer.push_back(page);
    }
    return found;
}

void CACHE::handle_prefetch_feedback()
{
    uint32_t this_epoch_accuracy = 0, acc_level = 0;

    cycle++;
    if(knob::measure_cache_acc && cycle >= next_measure_cycle)
    {
        this_epoch_accuracy = pf_filled_epoch ? 100*(float)pf_useful_epoch/pf_filled_epoch : 0; 
        pref_acc = (pref_acc + this_epoch_accuracy) / 2; // have some hysterisis
        acc_level = (pref_acc / ((float)100/CACHE_ACC_LEVELS)); // quantize into 8 buckets
        if(acc_level >= CACHE_ACC_LEVELS) acc_level = (CACHE_ACC_LEVELS - 1); // corner cases

        pf_useful_epoch = 0;
        pf_filled_epoch = 0;
        next_measure_cycle = cycle + knob::measure_cache_acc_epoch;

        total_acc_epochs++;
        acc_epoch_hist[acc_level]++;

        broadcast_acc(acc_level);
    }
}

void CACHE::broadcast_acc(uint32_t acc_level)
{
    /* boradcast to all the attached prefetchers */
    switch(cache_type)
    {
        case IS_L1I:    return; 
        case IS_L1D:    return l1d_prefetcher_broadcast_acc(acc_level);
        case IS_L2C:    return l2c_prefetcher_broadcast_acc(acc_level);
        case IS_LLC:    return llc_prefetcher_broadcast_acc(acc_level);
    }
}

void CACHE::reconfigure(uint32_t sets, uint32_t ways, uint32_t latency)
{
    // Deallocate existing block array
    for (uint32_t i = 0; i < NUM_SET; i++) {
        delete[] block[i];
    }
    delete[] block;

    // Update parameters
    NUM_SET = sets;
    NUM_WAY = ways;
    NUM_LINE = sets * ways;
    LATENCY = latency;

    // Reallocate block array
    block = new BLOCK* [NUM_SET];
    for (uint32_t i = 0; i < NUM_SET; i++) {
        block[i] = new BLOCK[NUM_WAY];
        for (uint32_t j = 0; j < NUM_WAY; j++) {
            block[i][j].lru = j;
        }
    }
}