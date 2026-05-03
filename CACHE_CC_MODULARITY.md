# Cache.cc Modularity & Readability Improvements

## Overview

The `cache.cc` file (2698 lines) has been refactored to improve modularity, readability, and maintainability through clear section headers and logical organization.

## File Structure

### Header Section (Lines 1-30)
Comprehensive module documentation explaining:
- Purpose of cache.cc (multi-level cache hierarchy)
- Key functional sections
- Integration with Pravesh PTW (Page Table Walker)
- Reference to page table handling code

### Main Sections

#### 1. Configuration & Initialization (Line ~150)
```cpp
// =====================================================================
// CONFIGURATION & INITIALIZATION
// =====================================================================
```
**Functions:**
- `print_cache_config()` - Print cache configuration
- `create_rq()` - Create read queue with appropriate priority scheme

**Purpose:** Initialize cache system with proper queue types and configuration

---

#### 2. Cache Fill Operations (Line ~190)
```cpp
// =====================================================================
// CACHE FILL OPERATIONS - Complete cache fill from next level
// =====================================================================
```
**Functions:**
- `handle_fill()` - Complete cache fills from next level

**Purpose:** Process data returns from lower levels and fill cache blocks

---

#### 3. Write-Back Operations (Line ~510)
```cpp
// =====================================================================
// WRITE-BACK OPERATIONS - Flush modified cache lines to next level
// =====================================================================
```
**Functions:**
- `handle_writeback()` - Flush dirty cache lines to next level

**Purpose:** Write dirty cache lines down the hierarchy

---

#### 4. Read Operations (Line ~880)
```cpp
// =====================================================================
// READ OPERATIONS - Process read requests and cache hits/misses
// =====================================================================
```
**Functions:**
- `handle_read()` - Process read requests
- `check_hit()` - Check for cache hit

**Purpose:** Handle incoming read requests and determine hit/miss

---

#### 5. Prefetch Operations (Line ~1315)
```cpp
// =====================================================================
// PREFETCH OPERATIONS - Handle prefetch requests
// =====================================================================
```
**Functions:**
- `handle_prefetch()` - Handle prefetch requests
- `prefetch_line()` - Insert prefetched lines
- `kpc_prefetch_line()` - KPC-style prefetch

**Purpose:** Manage prefetch requests from prefetcher engines

---

#### 6. Main Simulation Cycle (Line ~1590)
```cpp
// =====================================================================
// MAIN SIMULATION CYCLE - Orchestrates all cache operations
// =====================================================================
```
**Functions:**
- `operate()` - Main cycle orchestrator

**Purpose:** Coordinate all cache operations each cycle

---

#### 7. Cache Utility Functions (Line ~1598)
```cpp
// =====================================================================
// CACHE UTILITY FUNCTIONS - Address mapping, set/way computation
// =====================================================================
```
**Functions:**
- `get_set()` - Compute set index from address
- `get_way()` - Find way given address and set
- `fill_cache()` - Fill a specific cache block

**Purpose:** Low-level cache operations

---

#### 8. Cache Invalidation & Request Queue (Line ~1758)
```cpp
// =====================================================================
// CACHE INVALIDATION & REQUEST QUEUE MANAGEMENT
// =====================================================================
```
**Functions:**
- `invalidate_entry()` - Invalidate cache entry
- `add_rq()` - Add to read queue
- `add_wq()` - Add to write queue
- `add_pq()` - Add to prefetch queue

**Purpose:** Manage all request queues and cache invalidation

---

#### 9. Response Handling (Line ~2240)
```cpp
// =====================================================================
// RESPONSE HANDLING - Complete requests and route responses upward
// =====================================================================
// Pravesh: This section includes PTW page table walker integration
//
```
**Functions:**
- `return_data()` - Process responses and route upward
- `update_fill_cycle()` - Update next fill cycle

**Purpose:** Complete requests and route responses, with special handling for TRANSLATION packets

**Pravesh Integration:**
- Routes TRANSLATION packets to PTW via `handle_memory_response()`
- Enables proper caching through Page Walk Cache (PWC)

---

#### 10. MSHR Management (Line ~2318)
```cpp
// =====================================================================
// MSHR MANAGEMENT - Miss Status Holding Register operations
// =====================================================================
```
**Functions:**
- `check_mshr()` - Search MSHR for entry
- `add_mshr()` - Add entry to MSHR

**Purpose:** Manage in-flight requests and MSHR operations

---

## Key Improvements

### 1. Clear Section Boundaries
- 10 well-defined sections with clear separation
- Each section marked with distinctive header boxes
- Easy to navigate large file

### 2. Function Grouping
- Related functions grouped logically
- Section comments explain purpose
- Clear dependencies and data flows

### 3. Pravesh PTW Integration Documentation
- Highlighted at file header
- Marked in response handling section
- Comments explain TRANSLATION packet routing

### 4. Readability Enhancements
- Consistent header format
- Self-documenting section organization
- Clear function purpose statements

### 5. Maintainability
- Easy to locate specific functionality
- Clear dependencies between sections
- Simplified code review and debugging

## Navigation Guide

To find specific functionality:

| Functionality | Section | Line |
|---|---|---|
| Cache initialization | Configuration & Initialization | ~150 |
| Cache hits/misses | Read Operations | ~880 |
| Dirty line writes | Write-Back Operations | ~510 |
| Data returns | Fill Operations | ~190 |
| Prefetch handling | Prefetch Operations | ~1315 |
| Main cycle | Main Simulation Cycle | ~1590 |
| Address computation | Cache Utility Functions | ~1598 |
| Queue management | Request Queue Management | ~1758 |
| Response routing | Response Handling | ~2240 |
| In-flight requests | MSHR Management | ~2318 |

## Pravesh Page Table Walker Integration

The file includes special handling for page table TRANSLATION packets:

1. **L1D Detection** (Line ~445): TRANSLATION packets marked with `from_ptw = 1` flag
2. **Response Routing** (Line ~2215): Responses checked for `from_ptw` flag
3. **PTW Notification** (Line ~2217): Calls `page_table_walker->handle_memory_response()`
4. **PWC Caching**: PTW processes translation and caches in Page Walk Cache (PWC)

See comments marked with "Pravesh:" for specific implementation details.

## Conclusion

These improvements make `cache.cc` more maintainable and easier to understand while preserving all functionality. The clear modular structure facilitates:
- Faster code review
- Easier debugging and testing
- Better documentation
- Reduced development time for future enhancements
