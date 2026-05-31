#include "tracereader.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <type_traits>

static_assert(sizeof(context_instr) == 88, "context_instr size mismatch");

static_assert(offsetof(context_instr, destination_memory) == 0);
static_assert(offsetof(context_instr, source_memory) == 16);
static_assert(offsetof(context_instr, ip) == 48);
static_assert(offsetof(context_instr, magic) == 56);
static_assert(offsetof(context_instr, window_id) == 64);
static_assert(offsetof(context_instr, record_size) == 72);
static_assert(offsetof(context_instr, is_branch) == 76);
static_assert(offsetof(context_instr, branch_taken) == 77);
static_assert(offsetof(context_instr, destination_registers) == 78);
static_assert(offsetof(context_instr, source_registers) == 80);
static_assert(offsetof(context_instr, trace_window) == 84);


extern int KNOB_LIVE_INPUT;

tracereader::tracereader(uint8_t cpu, std::string _ts, bool live_trace) : cpu(cpu), trace_string(_ts), live_trace(live_trace)
{
  if(!live_trace)
  {
    std::string last_dot = trace_string.substr(trace_string.find_last_of("."));

    if (trace_string.substr(0, 4) == "http") {
      // Check file exists
      char testfile_command[4096];
      sprintf(testfile_command, "wget -q --spider %s", trace_string.c_str());
      FILE* testfile = popen(testfile_command, "r");
      if (pclose(testfile)) {
        std::cerr << "TRACE FILE NOT FOUND" << std::endl;
        assert(0);
      }
      cmd_fmtstr = "wget -qO- -o /dev/null %2$s | %1$s -dc";
    } else {
      std::ifstream testfile(trace_string);
      if (!testfile.good()) {
        std::cerr << "TRACE FILE NOT FOUND" << std::endl;
        assert(0);
      }
      cmd_fmtstr = "%1$s -dc %2$s";
    }

    std::cout << "last dot " << last_dot << '\n';
    if (last_dot[1] == 'g') // gzip format
      decomp_program = "gzip";
    else if (last_dot[1] == 'x') // xz
      decomp_program = "xz";
    else if (last_dot[1] == 'z') // for zip
      decomp_program = "unzip";
    else {
      std::cout << "ChampSim does not support traces other than gz or xz compression!" << std::endl;
      assert(0);
    }
    trace_open(trace_string);
  }
  else
  {
    trace_file = fopen(trace_string.c_str(), "r");
    if (trace_file == NULL) {
      std::cerr << std::endl << "*** CANNOT OPEN TRACE FILE: " << trace_string << " ***" << std::endl;
      assert(0);
    }
  }
  
}

tracereader::~tracereader() { close(); }

template <typename T>
ooo_model_instr tracereader::read_single_instr()
{
  
  T trace_read_instr;
  if(!live_trace)
  {
    while (!fread(&trace_read_instr, sizeof(T), 1, trace_file)) {
      // reached end of file for this trace
      std::cout << "*** Reached end of trace: " << trace_string << std::endl;
  
      // close the trace file and re-open it
      close();
      trace_open(trace_string);
      
    }
    ooo_model_instr retval(cpu, trace_read_instr);
    return retval;
  }
  else
  {
    
    context_instr h{};
    for (;;) 
    {
      int max_read_retry = 5;
      int read_again = 0;

      h = context_instr();
      if (fread(&h, sizeof(context_instr), 1, trace_file) != 1) {
        std::cerr << "Failed to read trace header\n";
        std::exit(1);
      }

      if(h.magic != MAGIC) {
        std::cerr << "Bad magic: stream not aligned / stdout contaminated\nRead again counter, " << read_again << '\n';
      }

      if (h.record_size != sizeof(context_instr)) {
        std::cerr << "Record size mismatch: producer=" << h.record_size
                  << " consumer=" << sizeof(context_instr) << "\n";
      }

      if (feof(trace_file)) {
        // producer ended cleanly: restart
        std::cerr << "producer ended cleanly: restart\n";
        close(); // pclose
        exit(1);
        // trace_file = popen(trace_string.c_str(), "r");
        // if (!trace_file) { perror("popen"); std::exit(1); }
        // continue;              // retry read
      }
      else if (ferror(trace_file)) {
        std::cerr << "not-eof --> read error/corruption\n";
      // not EOF => real error/corruption
        perror("fread"); 
      } 
      
      ooo_model_instr retval(cpu, h);
      return retval;
    }
  }
}

void tracereader::trace_open(std::string trace_string, int app)
{
//   std::cout << "XXXXXXXXXXXXXXXx FILE NAME " << trace_string << '\n';
//   int fd = open(trace_string.c_str(), O_RDWR, 0666);
//   buf = (shared_buffer*) mmap(NULL, sizeof(shared_buffer),
//                                             PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
//   if (buf == MAP_FAILED) {
//       perror("mmap failed");
//       return;
//   }
}

void tracereader::trace_open(std::string trace_string)
{
  char gunzip_command[4096];
  sprintf(gunzip_command, cmd_fmtstr.c_str(), decomp_program.c_str(), trace_string.c_str());
  trace_file = popen(gunzip_command, "r");
  if (trace_file == NULL) {
    std::cerr << std::endl << "*** CANNOT OPEN TRACE FILE: " << trace_string << " ***" << std::endl;
    assert(0);
  }
}

void tracereader::close()
{
  if (trace_file != NULL) {
    pclose(trace_file);
  }
}

class cloudsuite_tracereader : public tracereader
{
  ooo_model_instr last_instr;
  bool initialized = false;

public:
  cloudsuite_tracereader(uint8_t cpu, std::string _tn) : tracereader(cpu, _tn) {}

  ooo_model_instr get()
  {
    ooo_model_instr trace_read_instr = read_single_instr<cloudsuite_instr>();

    if (!initialized) {
      last_instr = trace_read_instr;
      initialized = true;
    }

    last_instr.branch_target = trace_read_instr.ip;
    ooo_model_instr retval = last_instr;

    last_instr = trace_read_instr;
    return retval;
  }
};

class input_tracereader : public tracereader
{
  ooo_model_instr last_instr;
  bool initialized = false;

public:
  input_tracereader(uint8_t cpu, std::string _tn, bool live_traces) : tracereader(cpu, _tn, live_traces) {}

  
  ooo_model_instr get()
  {
    ooo_model_instr trace_read_instr = read_single_instr<input_instr>();

    if (!initialized) {
      last_instr = trace_read_instr;
      initialized = true;
    }

    last_instr.branch_target = trace_read_instr.ip;
    ooo_model_instr retval = last_instr;

    last_instr = trace_read_instr;
    return retval;
  }
};

class context_tracereader : public tracereader
{
  ooo_model_instr last_instr;
  bool initialized = false;

public:
  context_tracereader(uint8_t cpu, std::string _tn, bool live_traces) : tracereader(cpu, _tn, live_traces) {}

  
  ooo_model_instr get()
  {
    ooo_model_instr trace_read_instr = read_single_instr<context_instr>();
    if (!initialized) {
      last_instr = trace_read_instr;
      initialized = true;
    }
      
    last_instr.branch_target = trace_read_instr.ip;
    ooo_model_instr retval = last_instr;

    last_instr = trace_read_instr;
    return retval;
  }
};

tracereader* get_tracereader(std::string fname, uint8_t cpu, bool is_cloudsuite, bool live_traces)
{
  if (is_cloudsuite) {
    return new cloudsuite_tracereader(cpu, fname);
  } else if(live_traces){
    return new context_tracereader(cpu, fname, live_traces);
  }
  else {
    return new input_tracereader(cpu, fname, live_traces);
  }
}
