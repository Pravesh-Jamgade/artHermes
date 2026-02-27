# STLB-PTW Integration - Quick Reference Guide

## Summary of Changes

This document provides a quick reference for the STLB-PTW integration implementation.

## Files Created/Modified

### New Files Created

1. **inc/ptw.h** - PTW module header
   - PTW class definition
   - 4-level PwC structures
   - PTW statistics and configuration

2. **src/ptw.cc** - PTW module implementation
   - Page walk logic
   - PwC lookup/insert/invalidate
   - LRU replacement policy
   - Statistics collection

3. **inc/stlb_ptw_integration.h** - Integration API header
   - Function declarations for STLB-PTW interface

4. **src/stlb_ptw_integration.cc** - Integration API
   - handle_stlb_miss() - Route STLB misses to PTW
   - stlb_ptw_lookup() - Search PwC
   - stlb_ptw_insert() - Insert into PwC
   - stlb_ptw_invalidate() - Invalidate entries
   - stlb_ptw_flush() - Flush all entries
   - get_ptw_stats() - Retrieve statistics

5. **PTW_STLB_INTEGRATION.md** - Comprehensive documentation

### Modified Files

1. **inc/ooo_cpu.h**
   - Added `#include "ptw.h"` 
   - Added `PTW *page_table_walker` member variable
   - Added `void operate_ptw()` method declaration

2. **src/ooo_cpu.cc**
   - Modified `initialize_core()` to create PTW instance
   - Added `operate_ptw()` method to process outstanding walks

## PTW 4-Level Page Walk Cache Architecture

```
┌─────────────────────────────────────────┐
│ PTW Module (4-Level Page Walk Cache)    │
├─────────────────────────────────────────┤
│ Level 0 (PML4):  64 sets ×  4 ways      │  Top-level page map
│ Level 1 (PDPT): 128 sets ×  4 ways      │  Page directory pointer table
│ Level 2 (PD):   256 sets ×  8 ways      │  Page directory
│ Level 3 (PT):   512 sets ×  8 ways      │  Page table entries
│                                         │
│ Outstanding Walk Queue: Max 16 walks    │
│ Each supports: Latency simulation       │
│                Translation caching      │
│                LRU replacement          │
└─────────────────────────────────────────┘
```

## Key Components

### 1. PwC_Entry Structure
```cpp
struct PwC_Entry {
    uint64_t vaddr;      // Virtual address of page table entry
    uint64_t paddr;      // Physical address (translation result)
    uint64_t tag;        // Tag for set-associative lookup
    uint32_t lru;        // LRU counter
    uint8_t valid;       // Valid bit
    uint32_t level;      // Which level (0-3)
};
```

### 2. PageWalkCacheLevel Structure
```cpp
struct PageWalkCacheLevel {
    uint32_t sets;       // Number of sets
    uint32_t ways;       // Number of ways per set
    vector<vector<PwC_Entry>> entries;  // Actual cache entries
    uint64_t hits;       // Hit counter
    uint64_t misses;     // Miss counter
    uint64_t replacements;  // Replacement counter
};
```

### 3. OutstandingWalk Structure
```cpp
struct OutstandingWalk {
    uint64_t vaddr;          // Virtual address being walked
    uint64_t current_level;  // Current page table level (0-3)
    uint64_t current_pa;     // Physical address of current page table
    uint64_t requested_cycle;// Cycle when walk started
    uint32_t instr_id;       // Instruction ID
    PACKET *packet;          // Associated packet
};
```

## Usage in Simulation

### During Simulation Loop

```cpp
// In main simulation loop
for each cycle:
    // ... (normal operations)
    
    // Process page walks
    for each CPU:
        cpu->operate_ptw();  // Handle outstanding walks
    
    // ... (other operations)
```

### Handling STLB Miss

```cpp
// When STLB misses
if (stlb_miss && ptw->is_walk_queue_available()) {
    ptw->initiate_page_walk(packet, virtual_address);
}
```

### PwC Lookup

```cpp
uint64_t paddr;
if (ptw->pwc_lookup(vaddr, level, paddr)) {
    // Cache hit! Use the cached translation
} else {
    // Cache miss, need full page table walk
}
```

## Configuration Parameters

### Latency
- `PTW_LATENCY = 100` cycles per page walk

### Queue Size
- `MAX_OUTSTANDING_WALKS = 16` concurrent walks

### Cache Dimensions
- Level 0 (PML4): 64 × 4 = 256 entries
- Level 1 (PDPT): 128 × 4 = 512 entries
- Level 2 (PD): 256 × 8 = 2048 entries
- Level 3 (PT): 512 × 8 = 4096 entries

## Statistics Available

After simulation, the PTW provides:

- **total_walks_initiated** - Number of page walks started
- **walks_completed** - Number of walks that finished
- **walks_stalled** - Number of walks that stalled
- **pwc_lX_hits/misses** - Per-level hit/miss counts
- **total_latency** - Accumulated walk latencies
- **max_latency** - Longest single walk latency

Access via: `cpu->page_table_walker->stats`

## Compilation

Include the new files in your build system:

```makefile
SOURCES += src/ptw.cc
SOURCES += src/stlb_ptw_integration.cc
INCLUDES += inc/ptw.h
INCLUDES += inc/stlb_ptw_integration.h
```

## Example Output

```
=== PTW Configuration ===
CPU: 0
PTW Latency: 100 cycles
Max Outstanding Walks: 16

Page Walk Cache (PwC) Configuration:
Level 4 (PML4): 64 sets x 4 ways
Level 3 (PDPT): 128 sets x 4 ways
Level 2 (PD):   256 sets x 8 ways
Level 1 (PT):   512 sets x 8 ways

=== PTW Statistics (CPU 0) ===
Total Page Walks Initiated: 10000
Walks Completed: 9995
Walks Stalled: 5
Average Walk Latency: 92.5 cycles
Max Walk Latency: 150 cycles

Page Walk Cache Statistics:
Level 4 (PML4): 3500 hits, 2000 misses (Hit Rate: 63.6%)
Level 3 (PDPT): 4200 hits, 1800 misses (Hit Rate: 70.0%)
Level 2 (PD):   4800 hits, 1200 misses (Hit Rate: 80.0%)
Level 1 (PT):   5200 hits, 800 misses (Hit Rate: 86.7%)

Total PwC Replacements: 4200
```

## Integration Checklist

- [x] Created PTW header (ptw.h)
- [x] Created PTW implementation (ptw.cc)
- [x] Added PTW to O3_CPU class
- [x] Created integration API header (stlb_ptw_integration.h)
- [x] Created integration API implementation (stlb_ptw_integration.cc)
- [x] Modified O3_CPU initialize_core()
- [x] Added operate_ptw() to O3_CPU
- [x] Created comprehensive documentation

## Next Steps

1. **Update STLB handling code** to call handle_stlb_miss() on misses
2. **Integrate main simulation loop** to call operate_ptw()
3. **Test with sample workloads** to verify correct operation
4. **Tune PwC parameters** based on workload characteristics
5. **Collect and analyze statistics** to evaluate performance

## Support

For issues or questions:
- Review PTW_STLB_INTEGRATION.md for detailed documentation
- Check ptw.h for API documentation
- Examine ptw.cc for implementation details
