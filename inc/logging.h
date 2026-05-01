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

// =====================================================================
// RUNTIME CONTROLS - Can be modified in your code
// =====================================================================

namespace logging {
    // Global enable/disable switch
    extern bool enabled;
    
    // Per-level controls
    extern bool debug_enabled;
    extern bool info_enabled;
    extern bool warn_enabled;
    extern bool error_enabled;
    
    // Show timestamp in logs
    extern bool show_timestamp;
    
    // Show file:line information
    extern bool show_location;
}

// =====================================================================
// INTERNAL LOGGING FUNCTIONS
// =====================================================================

namespace logging {
    // Get basename of file path
    inline const char* basename(const char* path) {
        const char* base = strrchr(path, '/');
        return base ? (base + 1) : path;
    }
    
    // Core logging function
    inline void log_message(const char* level, const char* file, int line, 
                           const char* fmt, va_list args) {
        if (!enabled) return;
        
        // Print level
        std::cout << "[" << level << "] ";
        
        // Print location if enabled
        if (show_location) {
            std::cout << basename(file) << ":" << line << " ";
        }
        
        // Print message
        char buffer[4096];
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        std::cout << buffer << std::endl;
    }
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

// =====================================================================
// LOGGING MACROS - Main API
// =====================================================================

#ifdef ENABLE_LOGGING

// Debug level - detailed debugging information
#define LOG_DEBUG(fmt, ...) \
    do { \
        if (logging::enabled && logging::debug_enabled) { \
            std::cout << "[DEBUG] " << __FILE__ << ":" << __LINE__ << " "; \
            printf(fmt, ##__VA_ARGS__); \
            std::cout << std::endl; \
        } \
    } while(0)

// Info level - general information
#define LOG_INFO(fmt, ...) \
    do { \
        if (logging::enabled && logging::info_enabled) { \
            std::cout << "[INFO] "; \
            if (logging::show_location) std::cout << logging::basename(__FILE__) << ":" << __LINE__ << " "; \
            printf(fmt, ##__VA_ARGS__); \
            std::cout << std::endl; \
        } \
    } while(0)

// Warning level - potential issues
#define LOG_WARN(fmt, ...) \
    do { \
        if (logging::enabled && logging::warn_enabled) { \
            std::cout << "[WARN] "; \
            if (logging::show_location) std::cout << logging::basename(__FILE__) << ":" << __LINE__ << " "; \
            printf(fmt, ##__VA_ARGS__); \
            std::cout << std::endl; \
        } \
    } while(0)

// Error level - serious problems
#define LOG_ERROR(fmt, ...) \
    do { \
        if (logging::enabled && logging::error_enabled) { \
            std::cerr << "[ERROR] " << logging::basename(__FILE__) << ":" << __LINE__ << " "; \
            fprintf(stderr, fmt, ##__VA_ARGS__); \
            std::cerr << std::endl; \
        } \
    } while(0)

// Conditional logging - only log if condition is true
#define LOG_IF(condition, level, fmt, ...) \
    do { \
        if (condition) { \
            LOG_##level(fmt, ##__VA_ARGS__); \
        } \
    } while(0)

// Pravesh: Specialized logging for page table operations
#define LOG_PTW(fmt, ...) \
    do { \
        if (logging::enabled && logging::debug_enabled) { \
            std::cout << "[PTW] " << logging::basename(__FILE__) << ":" << __LINE__ << " "; \
            printf(fmt, ##__VA_ARGS__); \
            std::cout << std::endl; \
        } \
    } while(0)

// Cache-specific logging
#define LOG_CACHE(cache_name, fmt, ...) \
    do { \
        if (logging::enabled && logging::debug_enabled) { \
            std::cout << "[" << cache_name << "] "; \
            printf(fmt, ##__VA_ARGS__); \
            std::cout << std::endl; \
        } \
    } while(0)

// Hex address logging helper
#define LOG_ADDR(msg, addr) \
    LOG_DEBUG("%s: 0x%lx", msg, (uint64_t)(addr))

#else  // ENABLE_LOGGING not defined

// No-op macros when logging is disabled (zero overhead)
#define LOG_DEBUG(fmt, ...)       do {} while(0)
#define LOG_INFO(fmt, ...)        do {} while(0)
#define LOG_WARN(fmt, ...)        do {} while(0)
#define LOG_ERROR(fmt, ...)       do {} while(0)
#define LOG_IF(cond, lvl, fmt, ...)  do {} while(0)
#define LOG_PTW(fmt, ...)         do {} while(0)
#define LOG_CACHE(name, fmt, ...) do {} while(0)
#define LOG_ADDR(msg, addr)       do {} while(0)

#endif // ENABLE_LOGGING

// =====================================================================
// HELPER MACROS
// =====================================================================

// Log function entry/exit for debugging
#define LOG_FUNC_ENTRY() LOG_DEBUG(">>> Entering %s", __func__)
#define LOG_FUNC_EXIT()  LOG_DEBUG("<<< Exiting %s", __func__)

// Assert with logging
#define LOG_ASSERT(condition, fmt, ...) \
    do { \
        if (!(condition)) { \
            LOG_ERROR("ASSERTION FAILED: " fmt, ##__VA_ARGS__); \
            assert(condition); \
        } \
    } while(0)

#endif // LOGGING_H
