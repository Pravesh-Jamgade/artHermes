#ifndef STLB_PTW_INTEGRATION_H
#define STLB_PTW_INTEGRATION_H

#include <cstdint>

/**
 * STLB-PTW Integration Header
 * 
 * This module provides the interface between the Shared TLB (STLB) and
 * the Page Table Walker (PTW) with 4-level Page Walk Cache (PwC).
 */

// Forward declarations
class O3_CPU;
struct PACKET;

/**
 * Handle STLB miss by initiating a page walk
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @param packet: The memory access packet
 * @return: true if page walk was initiated successfully
 */
extern "C" bool handle_stlb_miss(void* cpu_ptr, PACKET* packet);

/**
 * Lookup translation in PwC
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @param vaddr: Virtual address to lookup
 * @param paddr: Output parameter for physical address
 * @return: true if translation found in PwC
 */
extern "C" bool stlb_ptw_lookup(void* cpu_ptr, uint64_t vaddr, uint64_t& paddr);

/**
 * Insert translation into PwC
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @param vaddr: Virtual address
 * @param paddr: Physical address
 * @param level: PwC level (0-3)
 */
extern "C" void stlb_ptw_insert(void* cpu_ptr, uint64_t vaddr, uint64_t paddr, uint32_t level);

/**
 * Invalidate translation in PwC
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @param vaddr: Virtual address
 * @param level: PwC level (0-3) or -1 for all levels
 */
extern "C" void stlb_ptw_invalidate(void* cpu_ptr, uint64_t vaddr, int level);

/**
 * Flush all PwC entries
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 */
extern "C" void stlb_ptw_flush(void* cpu_ptr);

/**
 * Get PTW statistics
 * 
 * @param cpu_ptr: Pointer to the O3_CPU object
 * @return: Pointer to PTW stats
 */
extern "C" void* get_ptw_stats(void* cpu_ptr);

#endif // STLB_PTW_INTEGRATION_H
