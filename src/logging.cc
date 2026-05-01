// =====================================================================
// LOGGING.CC - Implementation of logging system runtime controls
// =====================================================================

#include "logging.h"

namespace logging {
    // Global enable/disable switch
    bool enabled = true;
    
    // Per-level controls (initialized from defaults in header)
    bool debug_enabled = DEFAULT_LOG_DEBUG;
    bool info_enabled  = DEFAULT_LOG_INFO;
    bool warn_enabled  = DEFAULT_LOG_WARN;
    bool error_enabled = DEFAULT_LOG_ERROR;
    
    // Display options
    bool show_timestamp = false;
    bool show_location = true;
}

// Global stream-style logger — flag mirrors logging::enabled
Logger l(false);
Logger cache_logger(true);
Logger front_end_logger(true);