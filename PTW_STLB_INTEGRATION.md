# STLB-PTW Integration Documentation

## Overview

This document describes the connection between the **Shared TLB (STLB)** and the **Page Table Walker (PTW)** module with a 4-level **Page Walk Cache (PwC)**.

## Architecture

### Memory Translation Hierarchy

```
┌─────────────────────────────────────┐
│  Virtual Address                     │
└──────────────┬──────────────────────┘
               │
        ┌──────▼──────┐
        │   L1 TLB    │  (DTLB for data, ITLB for instructions)
        │  Per-Core   │  64-128 entries
        └──────┬──────┘
               │ Miss
        ┌──────▼──────┐
        │   STLB      │  (Shared TLB)
        │   512 sets  │  4-way associative
        │  4096 entries
        └──────┬──────┘
               │ Miss
        ┌──────▼──────────────────────┐
        │   PTW with 4-Level PwC      │
        └──────┬──────────────────────┘
               │
        ┌──────▼──────────────────────────────────────┐
        │  4-Level Page Walk Cache (PwC)             │
        │                                             │
        │  L0 (PML4):  64 sets  × 4 ways  = 256 entries  │
        │  L1 (PDPT): 128 sets  × 4 ways  = 512 entries  │
        │  L2 (PD):   256 sets  × 8 ways  = 2048 entries │
        │  L3 (PT):   512 sets  × 8 ways  = 4096 entries │
        └──────┬──────────────────────────────────────┘
               │
        ┌──────▼──────────────┐
        │  Page Frames        │  (DRAM simulation)
        │  4KB per frame      │
        └─────────────────────┘
```

### PTW Module Components

#### 1. Page Walk Cache (PwC) - 4 Levels

The PTW includes 4 levels of cache that correspond to x86-64 page table levels:

| Level | Name  | Purpose                      | Sets | Ways | Total |
|-------|-------|------------------------------|------|------|-------|
| 0     | PML4  | Page Map Level 4 (top)       | 64   | 4    | 256   |
| 1     | PDPT  | Page Directory Pointer Table | 128  | 4    | 512   |
| 2     | PD    | Page Directory               | 256  | 8    | 2048  |
| 3     | PT    | Page Table (leaf)            | 512  | 8    | 4096  |

Each entry in the PwC contains:
- Virtual address tag
- Cached physical address (page table entry)
- Valid bit
- LRU replacement state

#### 2. Outstanding Walk Queue

The PTW supports up to 16 concurrent page walks to handle pipelined requests.

Each outstanding walk tracks:
- Virtual address
- Current level being walked
- Physical address of current page table
- Requested cycle
- Instruction ID
- Associated packet

### Page Walk Process

1. **STLB Miss**: Request reaches STLB and misses
2. **PTW Initiation**: PTW initiates a page walk for the virtual address
3. **Level-by-Level Walk**: 
   - For each level (0 to 3):
     - Check PwC for cached translation
     - If hit: use cached result
     - If miss: simulate page table access and cache result
4. **Translation Complete**: Physical address returned to STLB
5. **STLB Update**: Result cached in STLB for future hits

### Key Features

#### Performance Optimization

- **Multi-level Caching**: PwC caches intermediate page table entries
- **Pipelined Walks**: Support for 16 concurrent page walks
- **Fast-path Hits**: Cached translations avoid full page table walks
- **LRU Replacement**: Efficient cache replacement policy

#### Address Translation Example (x86-64)

For a 48-bit virtual address (bits 0-47):

```
VA[47:39] → PML4 index (9 bits)
VA[38:30] → PDPT index (9 bits)
VA[29:21] → PD index   (9 bits)
VA[20:12] → PT index   (9 bits)
VA[11:0]  → Page offset (12 bits)
```

The PTW walks these 4 levels, checking the PwC at each step.

### Statistics Tracked

The PTW module tracks:

- Total page walks initiated
- Walks completed
- Walks stalled
- Hit/Miss rates for each PwC level
- Total latency across all walks
- Maximum single-walk latency
- Cache replacements per level

### Integration API

The `stlb_ptw_integration.cc` module provides the following interfaces:

#### 1. Handle STLB Miss
```cpp
bool handle_stlb_miss(void* cpu_ptr, PACKET* packet);
```
Routes an STLB miss to the PTW module.

#### 2. PwC Lookup
```cpp
bool stlb_ptw_lookup(void* cpu_ptr, uint64_t vaddr, uint64_t& paddr);
```
Searches the PwC for a cached translation.

#### 3. PwC Insert
```cpp
void stlb_ptw_insert(void* cpu_ptr, uint64_t vaddr, uint64_t paddr, uint32_t level);
```
Inserts a translation into the appropriate PwC level.

#### 4. PwC Invalidate
```cpp
void stlb_ptw_invalidate(void* cpu_ptr, uint64_t vaddr, int level);
```
Invalidates a translation (for TLB shootdown).

#### 5. PwC Flush
```cpp
void stlb_ptw_flush(void* cpu_ptr);
```
Flushes all PwC entries (for context switches).

## Configuration

### Knobs

The following configuration knobs can be set:

```cpp
PTW_LATENCY              = 100  // Base latency for page walk
MAX_OUTSTANDING_WALKS    = 16   // Maximum concurrent walks
```

### Sizes

```cpp
PWC_L4_SETS = 64    PWC_L4_WAYS = 4     // PML4
PWC_L3_SETS = 128   PWC_L3_WAYS = 4     // PDPT
PWC_L2_SETS = 256   PWC_L2_WAYS = 8     // PD
PWC_L1_SETS = 512   PWC_L1_WAYS = 8     // PT
```

## Implementation Details

### Files

1. **inc/ptw.h**: PTW module header with 4-level PwC definition
2. **src/ptw.cc**: PTW module implementation
3. **inc/stlb_ptw_integration.h**: Integration API header
4. **src/stlb_ptw_integration.cc**: Integration API implementation

### Integration Points

1. **O3_CPU class** (`inc/ooo_cpu.h`, `src/ooo_cpu.cc`):
   - Added `PTW *page_table_walker` member
   - Added `operate_ptw()` method called each cycle

2. **Main simulation loop**:
   - `operate_ptw()` is called to process outstanding page walks

### LRU Replacement Policy

Each PwC level uses an LRU policy:
- Invalid entries are filled first
- On LRU update: accessed entry gets LRU counter = 0
- Other entries' LRU counters increment
- Victim selection: highest LRU counter

## Statistics Output

When simulation ends, PTW prints:

```
=== PTW Statistics (CPU X) ===
Total Page Walks Initiated: N
Walks Completed: N
Walks Stalled: N
Average Walk Latency: X cycles
Max Walk Latency: X cycles

Page Walk Cache Statistics:
Level 4 (PML4): HITS hits, MISSES misses (Hit Rate: X%)
Level 3 (PDPT): HITS hits, MISSES misses (Hit Rate: X%)
Level 2 (PD):   HITS hits, MISSES misses (Hit Rate: X%)
Level 1 (PT):   HITS hits, MISSES misses (Hit Rate: X%)

Total PwC Replacements: N
```

## Future Enhancements

1. **Speculative Page Walk**: Pre-walk based on predictions
2. **Hierarchical PwC**: Integrate with L3 cache hierarchy
3. **Adaptive Sizing**: Dynamic PwC resizing based on workload
4. **Prefetching**: Intelligent prefetching of page table entries
5. **Hardware Assist**: Integration with TLB hardware refill mechanism

## References

- Intel 64 and IA-32 Architectures Software Developer's Manual
- "TLB Design for Superscalar Processors" (Ekman et al.)
- ARM Architecture Reference Manual (for comparison)
