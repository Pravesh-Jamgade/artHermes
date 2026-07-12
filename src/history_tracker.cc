#include "champsim.h"
#include "const.h"
#include <iostream>
#include <iomanip>
#include <cstring>
#include <assert.h>

const int HISTORY_LIMIT = 1000;
PacketHistoryEvent packet_history[HISTORY_LIMIT];
int history_head = 0;
int history_count = 0;

void add_history_event(uint64_t cycle, uint64_t instr_id, uint64_t virt_addr, uint64_t address, uint64_t full_addr, 
                            uint8_t type, const char* event_type, const char* cache_name, 
                            bool hit_in_wq, bool cache_hit, bool cache_miss, bool mshr_merge, 
                            uint64_t merged_with_instr_id, int hit_where, int ptw_level) {
    const char* type_name = "UNKNOWN";
    if (type == 0) type_name = "LOAD";
    else if (type == 1) type_name = "RFO";
    else if (type == 2) type_name = "PREFETCH";
    else if (type == 3) type_name = "WRITEBACK";
    else if (type == 4) type_name = "TRANSLATION";

    // Check for 0 address field
    if (address == 0 || full_addr == 0) {
        // Record it first before printing
        PacketHistoryEvent& ev = packet_history[history_head];
        ev.cycle = cycle;
        ev.instr_id = instr_id;
        ev.address = address;
        ev.full_addr = full_addr;
        strncpy(ev.type, type_name, sizeof(ev.type) - 1);
        ev.type[sizeof(ev.type) - 1] = '\0';
        strncpy(ev.event_type, event_type, sizeof(ev.event_type) - 1);
        ev.event_type[sizeof(ev.event_type) - 1] = '\0';
        strncpy(ev.cache_name, cache_name, sizeof(ev.cache_name) - 1);
        ev.cache_name[sizeof(ev.cache_name) - 1] = '\0';
        ev.hit_in_wq = hit_in_wq;
        ev.cache_hit = cache_hit;
        ev.cache_miss = cache_miss;
        ev.mshr_merge = mshr_merge;
        ev.merged_with_instr_id = merged_with_instr_id;
        ev.hit_where = hit_where;
        ev.ptw_level = ptw_level;

        history_head = (history_head + 1) % HISTORY_LIMIT;
        if (history_count < HISTORY_LIMIT) {
            history_count++;
        }
        
        std::cerr << "\n[ERROR] Packet with zero address field detected! event_type=" << event_type << " cache=" << cache_name << " instr_id=" << instr_id << "\n";
        print_history();
        assert(0 && "Packet address is zero");
    }

    PacketHistoryEvent& ev = packet_history[history_head];
    ev.cycle = cycle;
    ev.instr_id = instr_id;
    ev.virt_addr = virt_addr;
    ev.address = address;
    ev.full_addr = full_addr;
    strncpy(ev.type, type_name, sizeof(ev.type) - 1);
    ev.type[sizeof(ev.type) - 1] = '\0';
    strncpy(ev.event_type, event_type, sizeof(ev.event_type) - 1);
    ev.event_type[sizeof(ev.event_type) - 1] = '\0';
    strncpy(ev.cache_name, cache_name, sizeof(ev.cache_name) - 1);
    ev.cache_name[sizeof(ev.cache_name) - 1] = '\0';
    ev.hit_in_wq = hit_in_wq;
    ev.cache_hit = cache_hit;
    ev.cache_miss = cache_miss;
    ev.mshr_merge = mshr_merge;
    ev.merged_with_instr_id = merged_with_instr_id;
    ev.hit_where = hit_where;
    ev.ptw_level = ptw_level;

    history_head = (history_head + 1) % HISTORY_LIMIT;
    if (history_count < HISTORY_LIMIT) {
        history_count++;
    }
}

void print_history() {
    std::cout << "\n================ PACKET HISTORY EVENT DUMP (PAST EVENTS) ================\n";
    std::cout << std::left 
              << std::setw(6)  << "Idx"
              << std::setw(12) << "Cycle"
              << std::setw(12) << "Instr ID"
              << std::setw(16) << "Virt Addr"
              << std::setw(16) << "Address"
              << std::setw(16) << "Full Addr"
              << std::setw(12) << "Type"
              << std::setw(16) << "Cache"
              << std::setw(22) << "Event Type"
              << std::setw(6)  << "Lvl"
              << std::setw(8)  << "WQ Hit"
              << std::setw(10) << "Hit/Miss"
              << std::setw(12) << "MSHR Merge"
              << std::setw(15) << "Merged Instr"
              << std::setw(12) << "Hit Where" << "\n";
         
    int current = history_head;
    for (int i = 0; i < history_count; i++) {
        int idx = (current - history_count + i + HISTORY_LIMIT) % HISTORY_LIMIT;
        auto& ev = packet_history[idx];
        
        std::string hit_miss_str = "N/A";
        if (ev.cache_hit) hit_miss_str = "HIT";
        else if (ev.cache_miss) hit_miss_str = "MISS";
        
        std::string hit_where_str = "N/A";
        if (ev.hit_where < NumHitWheres) {
            hit_where_str = hit_where_names[ev.hit_where];
        }
        
        std::string lvl_str = "N/A";
        if (ev.ptw_level != -1) {
            lvl_str = std::to_string(ev.ptw_level);
        }

        std::cout << std::left
                  << std::setw(6)  << i
                  << std::setw(12) << ev.cycle
                  << std::setw(12) << ev.instr_id
                  << "0x" << std::setw(14) << std::hex << ev.virt_addr << std::dec
                  << "0x" << std::setw(14) << std::hex << ev.address << std::dec
                  << "0x" << std::setw(14) << std::hex << ev.full_addr << std::dec
                  << std::setw(12) << ev.type
                  << std::setw(16) << ev.cache_name
                  << std::setw(22) << ev.event_type
                  << std::setw(6)  << lvl_str
                  << std::setw(8)  << (ev.hit_in_wq ? "Yes" : "No")
                  << std::setw(10) << hit_miss_str
                  << std::setw(12) << (ev.mshr_merge ? "Yes" : "No")
                  << std::setw(15) << ev.merged_with_instr_id
                  << std::setw(12) << hit_where_str << "\n";
    }
    std::cout << "=============================================================================\n" << std::endl;
}
