#include "cache.h"
#include "ooo_cpu.h"
#include "ptw.h"
#include <iostream>

using namespace std;

/**
 * STLB-PTW Integration Module
 * 
 * This module handles the connection between the Shared TLB (STLB) and 
 * the Page Table Walker (PTW) module with 4-level Page Walk Cache (PwC).
 * 
 * When an STLB miss occurs, the request is forwarded to the PTW module
 * to perform a multi-level page table walk using the cached results from
 * the 4-level PwC.
 */

/**
 * Function: handle_stlb_miss
 * 
 * Handles an STLB miss by initiating a page walk through the PTW module.
 * This function checks the 4-level PwC first for cached translations,
 * and only performs a full walk if necessary.
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @param packet: The memory access packet that missed in STLB
 * @return: true if the request was successfully sent to PTW, false otherwise
 */
extern "C" bool handle_stlb_miss(void* cpu_ptr, PACKET* packet) {
    O3_CPU* cpu = (O3_CPU*)cpu_ptr;
    
    if (cpu == NULL || cpu->page_table_walker == NULL || packet == NULL) {
        return false;
    }
    
    PTWclass* ptw = (PTWclass*)cpu->page_table_walker;
    
    // first try to satisfy translation using L1 data cache
    {
        PACKET cache_pkt;
        memset(&cache_pkt, 0, sizeof(cache_pkt));
        cache_pkt.address = packet->full_addr >> LOG2_BLOCK_SIZE;
        cache_pkt.full_addr = packet->full_addr;
        cache_pkt.cpu = cpu->cpu;
        cache_pkt.is_data = 1;
        cache_pkt.type = LOAD;
        cache_pkt.instruction = 0;
        cache_pkt.tlb_access = 1;              // mark as translation request
        cache_pkt.fill_level = FILL_L1;
        cache_pkt.fill_l1d = 1;

        // enqueue request in L1D read queue so the search is modelled
        int rq_index = ooo_cpu[cpu->cpu].L1D.add_rq(&cache_pkt);
        (void)rq_index; // we don't need the index for logic

        // still probe for an immediate hit so we can respond synchronously
        int way = ooo_cpu[cpu->cpu].L1D.check_hit(&cache_pkt);
        if (way != -1) {
            uint32_t set = ooo_cpu[cpu->cpu].L1D.get_set(cache_pkt.address);
            uint64_t pte_val = ooo_cpu[cpu->cpu].L1D.block[set][way].data;
            // compute resulting physical page from PTE value
            uint64_t final_pa = pte_val; // assume pte_val already final physical page
            packet->data = final_pa >> LOG2_PAGE_SIZE;
            packet->hit_where = hit_where_t::PTW;
            cpu->STLB.return_data(packet);
            return true;
        }
    }

    // Pravesh: Comment out PTW initiate_page_walk code
    // Check if PTW queue is available
    if (!ptw->is_walk_queue_available()) {
        return false; // PTW queue is full, stall the request
    }
    
    // Initiate page walk in PTW using full virtual address (includes offset)
    ptw->initiate_page_walk(packet, packet->full_addr);
    
    return true; // Pravesh: PTW disabled - return false to fall back to default handling
}

/**
 * Function: stlb_ptw_lookup
 * 
 * Performs a lookup in the 4-level PwC caches to find a cached translation.
 * This is called before initiating a full page walk.
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @param vaddr: Virtual address to translate
 * @param paddr: Output parameter for physical address
 * @return: true if found in PwC, false otherwise
 */
extern "C" bool stlb_ptw_lookup(void* cpu_ptr, uint64_t vaddr, uint64_t& paddr) {
    O3_CPU* cpu = (O3_CPU*)cpu_ptr;
    
    if (cpu == NULL || cpu->page_table_walker == NULL) {
        return false;
    }
    
    PTWclass* ptw = (PTWclass*)cpu->page_table_walker;
    
    // Try to find the translation in any of the 4 PwC levels
    for (uint32_t level = 0; level < PWC_TOTAL_LEVELS; level++) {
        if (ptw->pwc_lookup(vaddr, level, paddr)) {
            return true;
        }
    }
    
    return false;
}

/**
 * Function: stlb_ptw_insert
 * 
 * Inserts a translation result into the appropriate level of the PwC.
 * This is called when a page walk completes to cache the results.
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @param vaddr: Virtual address of the translation
 * @param paddr: Physical address result
 * @param level: PwC level to insert into (0-3)
 */
extern "C" void stlb_ptw_insert(void* cpu_ptr, uint64_t vaddr, uint64_t paddr, uint32_t level) {
    O3_CPU* cpu = (O3_CPU*)cpu_ptr;
    
    if (cpu == NULL || cpu->page_table_walker == NULL) {
        return;
    }
    
    PTWclass* ptw = (PTWclass*)cpu->page_table_walker;
    
    // Validate level
    if (level >= PWC_TOTAL_LEVELS) {
        return;
    }
    
    // Insert translation into PwC
    ptw->pwc_insert(vaddr, paddr, level);
}

/**
 * Function: stlb_ptw_invalidate
 * 
 * Invalidates a translation in the PwC caches.
 * This is used for TLB shootdown operations.
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @param vaddr: Virtual address to invalidate
 * @param level: PwC level to invalidate from (0-3), or -1 for all levels
 */
extern "C" void stlb_ptw_invalidate(void* cpu_ptr, uint64_t vaddr, int level) {
    O3_CPU* cpu = (O3_CPU*)cpu_ptr;
    
    if (cpu == NULL || cpu->page_table_walker == NULL) {
        return;
    }
    
    PTWclass* ptw = (PTWclass*)cpu->page_table_walker;
    
    if (level < 0 || level >= PWC_TOTAL_LEVELS) {
        // Invalidate all levels
        ptw->pwc_flush();
    } else {
        // Invalidate specific level
        ptw->pwc_invalidate(vaddr, level);
    }
}

/**
 * Function: stlb_ptw_flush
 * 
 * Flushes all PwC entries. Used for context switches and global TLB flushes.
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 */
extern "C" void stlb_ptw_flush(void* cpu_ptr) {
    O3_CPU* cpu = (O3_CPU*)cpu_ptr;
    
    if (cpu == NULL || cpu->page_table_walker == NULL) {
        return;
    }
    
    PTWclass* ptw = (PTWclass*)cpu->page_table_walker;
    ptw->pwc_flush();
}

/**
 * Function: get_ptw_stats
 * 
 * Retrieves statistics from the PTW module.
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @return: Pointer to PTW stats structure
 */
extern "C" void* get_ptw_stats(void* cpu_ptr) {
    O3_CPU* cpu = (O3_CPU*)cpu_ptr;
    
    if (cpu == NULL || cpu->page_table_walker == NULL) {
        return NULL;
    }
    
    return (void*)(&cpu->page_table_walker->stats);
}

/**
 * STLB-PTW Integration Architecture:
 * 
 * 1. STLB Lookup Hierarchy:
 *    - L1: DTLB (per-core data TLB)
 *    - L2: ITLB (per-core instruction TLB)
 *    - L3: STLB (unified shared TLB)
 *    - L4: PTW with 4-level PwC (simulated page table walk)
 * 
 * 2. PTW 4-Level Page Walk Cache (PwC):
 *    - Level 0 (PML4): 64 sets x 4 ways - Top-level page map
 *    - Level 1 (PDPT): 128 sets x 4 ways - Page directory pointer table
 *    - Level 2 (PD):   256 sets x 8 ways - Page directory
 *    - Level 3 (PT):   512 sets x 8 ways - Page table entries
 * 
 * 3. Page Walk Process:
 *    - On STLB miss, check PwC for cached intermediate translations
 *    - Walk through each level, caching results in PwC for future hits
 *    - Return final physical address to STLB for caching
 *    - Pipelined walks support up to 16 concurrent page walks
 * 
 * 4. Cache Coherency:
 *    - TLB invalidations flush relevant PwC entries
 *    - Context switches flush all PwC entries
 *    - Address space isolation maintained through ASID
 */
