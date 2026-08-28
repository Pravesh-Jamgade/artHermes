#ifndef KNOBS_H
#define KNOBS_H

#include <vector>
#include <cstdint>
#include <string>
#include <utility>
#define MAX_LEN 256

extern std::vector<std::pair<std::string, std::string>> tracked_explicit_settings;
int parse_knobs_tracker(void* user, const char* section, const char* name, const char* value);

void parse_args(int argc, char* argv[]);
void parse_config(char *config_file_name);
int parse_knobs(void* user, const char* section, const char* name, const char* value);
int handler(void* user, const char* section, const char* name, const char* value);

/* auxiliary functions */
std::vector<int32_t> get_array_int(const char *str);
std::vector<float> get_array_float(const char *str);

#endif /* KNOBS_H */

namespace knob
{
    extern std::string footprint_track_type;
    extern uint32_t stlb_set;
    extern uint32_t stlb_way;
    extern uint32_t stlb_latency;
    extern uint32_t l2c_latency;
    extern uint32_t llc_latency;
    extern uint32_t itlb_latency;
    extern uint32_t dtlb_latency;
    extern uint32_t l1i_latency;
    extern uint32_t l1d_latency;
    extern std::string max_lru_before_eviction_block_type;
    // Mode for shadowSTLB. "analysis": parallel lookup and direct refill; "detail": shadowSTLB replaces STLB
    extern std::string shadowstlb_mode;
    extern uint32_t translation_extra_latency;
}