#include <cstdio>
#include <string>

#include "instruction.h"

class tracereader
{
protected:
  FILE* trace_file = NULL;
  uint8_t cpu;
  std::string cmd_fmtstr;
  std::string decomp_program;
  std::string trace_string;
//   shared_buffer* buf;
  uint64_t instr_count = 0;
  bool live_trace;
public:
  tracereader(const tracereader& other) = delete;
  tracereader(uint8_t cpu, std::string _ts, bool live_traces=false);
  ~tracereader();
  // void open(std::string trace_string);
  void trace_open(std::string trace_string);
  void trace_open(std::string trace_string, int app);
  void close();

  template <typename T>
  ooo_model_instr read_single_instr();

  bool read_header_or_die();

  virtual ooo_model_instr get() = 0;
};

tracereader* get_tracereader(std::string fname, uint8_t cpu, bool is_cloudsuite, bool live_traces=false);


