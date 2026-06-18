// =====================================================================
// LOGGING.H - Flexible Logging System for ChampSim
// =====================================================================
// 
// Usage:
//   #include "logging.h"
//   
//   // In your code:
//   LOG_DEBUG("Value: %d", some_value);
//   LOG_INFO("Processing cache at cycle: %lu", cycle);
//   LOG_WARN("MSHR occupancy high: %d/%d", occupancy, size);
//   LOG_ERROR("Invalid address: 0x%lx", addr);
//
// Enable/Disable:
//   - Compile-time: Comment out #define ENABLE_LOGGING
//   - Runtime: Set log_enabled = false in your code
//   - Per-level: Set specific level flags (log_debug_enabled, etc.)
//
// =====================================================================

#ifndef LOGGING_H
#define LOGGING_H

#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <inttypes.h>
#include <vector>
#include <algorithm>

// =====================================================================
// CONFIGURATION - Enable/Disable logging at compile time
// =====================================================================

// Comment out to disable ALL logging at compile time (zero overhead)
// #define ENABLE_LOGGING

// Default log levels enabled at runtime (can be changed dynamically)
#define DEFAULT_LOG_DEBUG   true
#define DEFAULT_LOG_INFO    true
#define DEFAULT_LOG_WARN    true
#define DEFAULT_LOG_ERROR   true

// =====================================================================
// HELPERS
// =====================================================================

// Convert any integer to a "0x..." hex string.
// Works with int, uint32_t, uint64_t, uintptr_t, etc.
// Usage: l.log("addr=", hex2str(addr), " size=", hex2str(size), '\n');
template<typename T>
inline std::string hex2str(T value) {
    std::ostringstream oss;
    oss << "0x" << std::hex << value;
    return oss.str();
}

struct PacketTrackerFilter {
    bool track_instr = false;
    bool track_addr = false;
    std::vector<uint64_t> instr_id;
    std::vector<uint64_t> ip;

    bool enabled() const { return track_instr || track_addr; }

    bool matches(uint64_t instr, uint64_t addr) const {
        bool instr_ok = !track_instr || std::find(instr_id.begin(), instr_id.end(), instr) != instr_id.end();
        bool addr_ok = !track_addr || std::find(ip.begin(), ip.end(), addr) != ip.end();
        return instr_ok && addr_ok;
    }
};

inline void parse_env_list(const char* env_str, std::vector<uint64_t>& out_list) {
    if (!env_str) return;
    
    std::stringstream ss(env_str);
    std::string item;
    
    // Splits by comma: "123,456,789"
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            out_list.push_back(std::strtoull(item.c_str(), nullptr, 0));
        }
    }
}

static PacketTrackerFilter get_packet_tracker_filter()
{
    PacketTrackerFilter f;
    if (const char *instr_env = std::getenv("TRACK_INSTR_ID")) {
        f.track_instr = true;
        parse_env_list(instr_env, f.instr_id);
        // std::cout << "Tracking packets for instruction ID: " << f.instr_id << std::endl;
    }
    if (const char *addr_env = std::getenv("TRACK_FULL_ADDR")) {
        f.track_addr = true;
        parse_env_list(addr_env, f.ip);
        // std::cout << "Tracking packets for full address: 0x" << std::hex << f.ip << std::dec << std::endl;
    }
  
    return f;
}


#include <source_location>
#include <type_traits>
#include <string>

// helper to check if type is iterable
template<typename T, typename = void>
struct is_iterable : std::false_type{};

template<typename T>
struct is_iterable<T, std::void_t< decltype(std::begin(std::declval<T&>())), decltype(std::end(std::declval<T&>()))> >: std::true_type {};

// ip, addr, Component NAME
template<typename T>
inline void doctor(const T& val1, const T& val2, std::string NAME, std::string msg, const std::source_location loc=std::source_location::current()) {
    PacketTrackerFilter f = get_packet_tracker_filter();
    if(f.matches(0,val1))
    std::cout << "Func: " << loc.function_name() << "," << loc.line() << "," << hex2str(val1) << "," << hex2str(val2) << "," << NAME << "," << msg << "\n";
}

// =====================================================================
// LOGGER CLASS - Stream-style variadic template logging
// =====================================================================
// Usage: l.log("push to stack ", '(', '\n');
//        l.log("addr=0x", std::hex, addr, " cycle=", cycle, '\n');
// Each argument is streamed with operator<<; no format string needed.
// =====================================================================

class Logger {
public:
    bool flag;

    Logger(bool enabled = false) : flag(enabled) {}

    // Base case: no arguments left — do nothing
    void log() {}

    // Recursive case: stream p, then recurse on the rest
    template<typename P, typename ...Param>
    void log(const P &p, const Param& ...param) {
        if (this->flag) {
            std::cout << p <<" ";
            log(param...);
        }
    }

    // Convenience: enable/disable at runtime
    void enable()  { flag = true;  }
    void disable() { flag = false; }
};

// Global instance — controlled by logging::enabled
extern Logger l;
extern Logger cache_logger;
extern Logger front_end_logger;
extern Logger o3_logger;
extern Logger debug;


#endif // LOGGING_H
