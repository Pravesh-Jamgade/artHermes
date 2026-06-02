#include "tracereader.h"

#include <cassert>
#include <vector>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <cerrno>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

tracereader::tracereader(uint8_t cpu, std::string _ts, bool app_driven) : cpu(cpu), trace_string(_ts), app_driven(app_driven)
{
  if(!app_driven)
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
  // else
  // {
  //   trace_file = fopen(trace_string.c_str(), "r");
  //   if (trace_file == NULL) {
  //     std::cerr << std::endl << "*** CANNOT OPEN TRACE FILE: " << trace_string << " ***" << std::endl;
  //     assert(0);
  //   }
  // }
  
}

tracereader::~tracereader() { close(); }

template <typename T>
ooo_model_instr tracereader::read_single_instr()
{
  
  T trace_read_instr;
  if(!app_driven)
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
    if (!initialized) {
      last_instr = read_single_instr<cloudsuite_instr>();
      initialized = true;
    }

    ooo_model_instr trace_read_instr = read_single_instr<cloudsuite_instr>();

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
  input_tracereader(uint8_t cpu, std::string _tn) : tracereader(cpu, _tn) {}

  
  ooo_model_instr get()
  {
    if (!initialized) {
      last_instr = read_single_instr<input_instr>();
      initialized = true;
    }

    ooo_model_instr trace_read_instr = read_single_instr<input_instr>();

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
    if (!initialized) {
      last_instr = read_single_instr<context_instr>();
      initialized = true;
    }

    ooo_model_instr trace_read_instr = read_single_instr<context_instr>();
      
    last_instr.branch_target = trace_read_instr.ip;
    ooo_model_instr retval = last_instr;

    last_instr = trace_read_instr;
    return retval;
  }
};

namespace knob {
  extern bool enable_app_driven;
  extern bool enable_libc_buffered;
  extern bool enable_shmem_buffered;
}

class libc_buffered_tracereader : public tracereader
{
  ooo_model_instr last_instr;
  bool initialized = false;
  std::vector<char> buffer;

public:
  libc_buffered_tracereader(uint8_t cpu, std::string _tn) : tracereader(cpu, _tn, true)
  {
    // open the trace file (this could be a FIFO or regular file)
    trace_file = fopen(trace_string.c_str(), "rb");
    if (!trace_file) {
      std::cerr << "Failed to open trace file/FIFO: " << trace_string << " (errno=" << errno << ")" << std::endl;
      std::exit(1);
    }
    // Set libc buffer strategy matching the producer (e.g. 1MB buffer)
    buffer.resize(1 << 20); // 1MB buffer
    if (setvbuf(trace_file, buffer.data(), _IOFBF, buffer.size()) != 0) {
      std::cerr << "Failed to setvbuf on trace file/FIFO" << std::endl;
    }
  }

  ~libc_buffered_tracereader()
  {
    if (trace_file) {
      fclose(trace_file);
      trace_file = nullptr;
    }
  }

  ooo_model_instr read_helper()
  {
    context_instr trace_read_instr{};
    size_t read_bytes = fread(&trace_read_instr, sizeof(context_instr), 1, trace_file);
    if (read_bytes != 1) {
      if (feof(trace_file)) {
        std::cerr << "Reached end of trace file/FIFO cleanly (EOF)." << std::endl;
        std::exit(0);
      } else {
        std::cerr << "Error reading trace file/FIFO (errno=" << errno << ")" << std::endl;
        std::exit(1);
      }
    }

    if (trace_read_instr.magic == 0x1111425411114345ULL) { // END_MAGIC
      std::cerr << "Reached end of trace file/FIFO cleanly (END_MAGIC)." << std::endl;
      std::exit(0);
    }

    if (trace_read_instr.magic != 0x544C425452414345ULL) { // MAGIC
      std::cerr << "Bad magic: stream not aligned / stdout contaminated. Read: 0x" 
                << std::hex << trace_read_instr.magic << std::dec << std::endl;
    }

    return ooo_model_instr(cpu, trace_read_instr);
  }

  ooo_model_instr get() override
  {
    if (!initialized) {
      last_instr = read_helper();
      initialized = true;
    }

    ooo_model_instr trace_read_instr = read_helper();

    last_instr.branch_target = trace_read_instr.ip;
    ooo_model_instr retval = last_instr;

    last_instr = trace_read_instr;
    return retval;
  }
};

struct SharedBuffer {
    volatile uint64_t head = 0;
    volatile uint64_t tail = 0;
    static constexpr size_t BUFFER_SIZE = 1024;
    context_instr buffer[BUFFER_SIZE];
};

class shmem_buffered : public tracereader
{
  SharedBuffer *shared_buffer_ptr = nullptr;
  ooo_model_instr last_instr;
  bool initialized = false;

public:
  shmem_buffered(uint8_t cpu, std::string _tn) : tracereader(cpu, _tn, true)
  {
    int shm_fd = shm_open(trace_string.c_str(), O_RDWR, 0666);
    if (shm_fd < 0) {
      std::cerr << "Failed to open shared memory: " << trace_string << " (errno=" << errno << ")" << std::endl;
      std::exit(1);
    }
    shared_buffer_ptr = (SharedBuffer*)mmap(NULL, sizeof(SharedBuffer), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_buffer_ptr == MAP_FAILED) {
      std::cerr << "Failed to mmap shared memory: " << trace_string << " (errno=" << errno << ")" << std::endl;
      std::exit(1);
    }
    ::close(shm_fd);
  }

  ~shmem_buffered()
  {
    if (shared_buffer_ptr && shared_buffer_ptr != MAP_FAILED) {
      munmap(shared_buffer_ptr, sizeof(SharedBuffer));
    }
  }

  ooo_model_instr read_helper()
  {
    while (shared_buffer_ptr->head == shared_buffer_ptr->tail) {
      #if defined(__x86_64__) || defined(_M_X64)
      asm volatile("pause" ::: "memory");
      #endif
    }

    context_instr trace_read_instr = shared_buffer_ptr->buffer[shared_buffer_ptr->head % SharedBuffer::BUFFER_SIZE];
    shared_buffer_ptr->head = shared_buffer_ptr->head + 1;

    return ooo_model_instr(cpu, trace_read_instr);
  }

  ooo_model_instr get() override
  {
    if (!initialized) {
      last_instr = read_helper();
      initialized = true;
    }

    ooo_model_instr trace_read_instr = read_helper();

    last_instr.branch_target = trace_read_instr.ip;
    ooo_model_instr retval = last_instr;

    last_instr = trace_read_instr;
    return retval;
  }
};

tracereader* get_tracereader(std::string fname, uint8_t cpu, bool is_cloudsuite)
{
  if (is_cloudsuite) {
    return new cloudsuite_tracereader(cpu, fname);
  } else if (knob::enable_app_driven) {
    if(knob::enable_shmem_buffered)
    {
      return new shmem_buffered(cpu, fname);
    }
    else if(knob::enable_libc_buffered)
    {
      return new libc_buffered_tracereader(cpu, fname);
    }
  }
  else {
    return new input_tracereader(cpu, fname);
  }
}
