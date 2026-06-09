#include "pin.H"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <ctime>
#include <iostream>
#include <csignal>
#include <cstdlib>

/*
tracer_new.cpp
*/

#define MAGIC 0x544C425452414345ULL
#define END_MAGIC 0x1111425411114345ULL
static constexpr int NUM_INSTR_DESTINATIONS = 2;
static constexpr int NUM_INSTR_SOURCES      = 4;

uint32_t knob_number_of_windows = 0;
uint64_t window_size = 0;
uint64_t window_id = 0;
uint64_t traceable_window = 0;
int instr_id = 0;
uint64_t instruction_count = 0;
volatile sig_atomic_t signal_received = 0;

// uint32_t id = 0;// 1 for store , 2 for load
struct context_instr {

  uint64_t destination_memory[NUM_INSTR_DESTINATIONS] = {};
  uint64_t source_memory[NUM_INSTR_SOURCES] = {};
  uint64_t ip = 0;
  uint64_t magic = 0;
  uint64_t window_id = 0;
  uint32_t record_size = 0;// 1 for store , 2 for load
  uint8_t is_branch = 0;
  uint8_t branch_taken = 0;
  uint8_t destination_registers[NUM_INSTR_DESTINATIONS] = {};
  uint8_t source_registers[NUM_INSTR_SOURCES] = {};
  uint8_t trace_window = 0;
};

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

using trace_instr_format_t = context_instr;

struct thread_state_t {
  bool should_write = 0;
  uint64_t instrCount = 0;
  trace_instr_format_t curr{};
  // std::ofstream outfile;
};

static TLS_KEY tls_key;
static PIN_LOCK lock;
static THREADID tracked_tid = INVALID_THREADID;

KNOB<UINT64> KnobSkipInstructions (KNOB_MODE_WRITEONCE, "pintool", "s", "0", "skip");
KNOB<UINT64> KnobTraceInstructions(KNOB_MODE_WRITEONCE, "pintool", "t", "1000000", "trace");
KNOB<UINT64> KnobMaxWindows(KNOB_MODE_WRITEONCE, "pintool", "w", "5", "maximum windows to trace");
KNOB<std::string> KnobPhaseFileInstructions(KNOB_MODE_WRITEONCE, "pintool", "phase_file", "default_ppki.csv", "read phase file");
KNOB<std::string> KnobOut(KNOB_MODE_WRITEONCE, "pintool", "o",
  "/tmp/champsim_trace.fifo", "trace output fifo/file");

// ---------- helper: insert unique into fixed array ----------

 // Convert integer to hex string
 std::string intToHex(uint64_t value) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase << value;
  return oss.str();
}

struct PhaseRow {
  int tid;
  int win_id;
  uint64_t insts;

  uint64_t uniq4k;
  double ppki4k;

  uint64_t new4k;
  double newpageski;
  double new_frac;

  double jac;
  double churn;

  int cluster_id;
  int hot;
};

std::vector<PhaseRow> phase_data;

static FILE* g_out = nullptr;

std::vector<PhaseRow> read_phase_csv(const std::string& filename)
{
    std::ifstream file(filename.c_str());
    if (!file) {
      std::cerr << "ERROR: Cannot open file: " << filename << std::endl;
      PIN_ExitProcess(1);
  }
    std::vector<PhaseRow> rows;
    std::vector<int> phase_cold;
    std::string line;

    // Skip header
    std::getline(file, line);

    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string tok;
        PhaseRow r;

        std::getline(ss, tok, ','); r.tid = ::atoi(tok.c_str());
        std::getline(ss, tok, ','); r.win_id = ::atoi(tok.c_str());
        std::getline(ss, tok, ','); r.insts = ::strtoull(tok.c_str(), nullptr, 10);

        std::getline(ss, tok, ','); r.uniq4k = ::strtoull(tok.c_str(), nullptr, 10);
        std::getline(ss, tok, ','); r.ppki4k = ::strtod(tok.c_str(), nullptr);

        std::getline(ss, tok, ','); r.new4k = ::strtoull(tok.c_str(), nullptr, 10);
        std::getline(ss, tok, ','); r.newpageski = ::strtod(tok.c_str(), nullptr);
        std::getline(ss, tok, ','); r.new_frac = ::strtod(tok.c_str(), nullptr);

        std::getline(ss, tok, ','); r.jac = ::strtod(tok.c_str(), nullptr);
        std::getline(ss, tok, ','); r.churn = ::strtod(tok.c_str(), nullptr);

        std::getline(ss, tok, ','); r.cluster_id = ::atoi(tok.c_str());
        std::getline(ss, tok, ','); r.hot = ::atoi(tok.c_str());

        // std::cerr << "Before win_id: " << r.win_id << ", hot, " << r.hot << ", instr, " << r.insts << '\n';

        window_size = r.insts;
        rows.push_back(r);

        if(r.hot == 0)
        {
          phase_cold.push_back(rows.size() - 1);
        }
    }

    if (rows.empty()) {
      std::cerr << "ERROR: No phase rows found in " << filename << std::endl;
      PIN_ExitProcess(1);
    }

    if (window_size == 0) {
      std::cerr << "ERROR: window_size is 0" << std::endl;
      PIN_ExitProcess(1);
    }

    knob_number_of_windows = KnobMaxWindows.Value();

    // if w > rows.size() then wrap around
    if(knob_number_of_windows > rows.size()){
      knob_number_of_windows = rows.size();
    }

    // for(auto entry: phase_cold)
    //   std::cout << "Cold phase win_id, " << entry << '\n';

    uint64_t hot_region_count = rows.size() - phase_cold.size();
    if(hot_region_count < knob_number_of_windows)
    {
      ::srand(::time(nullptr));

      int range = phase_cold.size();
      if (range > 0) {
        int kTimes = knob_number_of_windows - hot_region_count;
        while(kTimes--)
        {
          int random_num = ::rand() % range;
          int row_idx = phase_cold[random_num];

          rows[row_idx].hot = 1;
        }
      }
    }

    // for(auto entry: rows)
    // std::cout << "After win_id, " << entry.win_id << ", hot, " << entry.hot << '\n';


    file.close();

    return rows;
}



template <typename T>
static inline VOID WriteToSet(T* begin, T* end, T value)
{
  T* empty = std::find(begin, end, (T)0);
  if (std::find(begin, empty, value) != empty) return;
  if (empty != end) *empty = value;
}

// ---------- Analysis routines ----------
static inline thread_state_t* GetState(THREADID tid)
{
  return static_cast<thread_state_t*>(PIN_GetThreadData(tls_key, tid));
}


VOID ResetCurrentInstruction(THREADID tid, ADDRINT ip)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  st->curr = {};
  st->curr.ip = static_cast<uint64_t>(ip);
}

VOID BranchOrNot(THREADID tid, BOOL taken)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  st->curr.is_branch = 1;
  st->curr.branch_taken = taken ? 1 : 0;
}

BOOL ShouldWrite(THREADID tid)
{
  if (tid != tracked_tid) return FALSE;
  auto* st = GetState(tid);
  if (!st) return FALSE;

  ++st->instrCount;
  // const uint64_t n = st->instrCount;
  // const uint64_t start = KnobSkipInstructions.Value() + 1;
  // const uint64_t end   = KnobSkipInstructions.Value() + KnobTraceInstructions.Value();

  // // uint64_t temp_new_window_id = (st->instrCount-1) / (window_size);

  // // if(temp_new_window_id < phase_data.size())
  // // {
  // //   bool change_phase = false;
  // //   if(window_id != temp_new_window_id)
  // //   {
  // //     change_phase = true;
  // //     window_id = temp_new_window_id;
  // //     if(phase_data[window_id].hot == 1)
  // //     {
  // //       traceable_window++;
  // //     }
  // //   }
    
  // //   st->curr.trace_window = phase_data[window_id].hot;
  // //   st->curr.window_id = phase_data[window_id].win_id; 
    
  // //   if(change_phase)
  // //     fprintf(stderr, "Hot=%d, Win=%lu, Traced=%lu\n", st->curr.trace_window, st->curr.window_id, traceable_window);
  // // }
 
  // if (n > end || traceable_window > knob_number_of_windows) {
  //   std::cerr << "Exiting: (n > end || traceable_window > knob_number_of_windows || temp_new_window_id >= phase_data.size())" << '\n';
  //   PIN_ExitApplication(0);
  //   return FALSE;
  // }

  // bool allow_write = (n >= start && n <= end);
  // st->should_write = allow_write;
  // fprintf(stderr, "ShouldWrite %p, %lu, %lu, %lu, %d\n", (void*)st->curr.ip, start, n, end, allow_write);
  return 1; //phase_data[window_id].hot == 1 && 
}


VOID WriteCurrentInstruction(THREADID tid)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  // st->curr.record_size = sizeof(trace_instr_format_t);//valid instruction
  // st->curr.magic = MAGIC;
  // st->curr.window_id = window_id;

  // /*write to buffer fifo*/
  // if (!g_out) {
  //     fprintf(stderr, "g_out is NULL\n");
  //     PIN_ExitApplication(1);
  // }

  fwrite(&st->curr, sizeof(trace_instr_format_t), 1, g_out);

  // fprintf(stderr, "%p, %lu, %d, %d\n", (void*)st->curr.ip, st->instrCount, st->curr.trace_window, st->should_write);

  // if ((st->instrCount & 0xFFFF) == 0) fflush(stdout);

  // uint64_t total = KnobSkipInstructions.Value() + KnobTraceInstructions.Value();
  // bool should_stop = st->instrCount > total;
  // fprintf(stderr, "%p, %d, %ld, %d, %ld, %ld, %d\n", (void*)st->curr.ip, st->curr.record_size, instruction_count, tid, st->instrCount, total, should_stop);
  // // flushing every instruction is expensive; flush occasionally if needed
  // if ((st->instrCount & 0xFFFF) == 0) fflush(stdout);
}

// Register helpers
VOID AddRegRead(THREADID tid, UINT32 reg)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  WriteToSet<uint8_t>(st->curr.source_registers,
                      st->curr.source_registers + NUM_INSTR_SOURCES,
                      static_cast<uint8_t>(reg));
}

VOID AddRegWrite(THREADID tid, UINT32 reg)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  WriteToSet<uint8_t>(st->curr.destination_registers,
                      st->curr.destination_registers + NUM_INSTR_DESTINATIONS,
                      static_cast<uint8_t>(reg));
}

// Memory helpers (ADDRINT / 64-bit)
VOID AddMemRead(THREADID tid, ADDRINT ea)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  WriteToSet<uint64_t>(st->curr.source_memory,
                       st->curr.source_memory + NUM_INSTR_SOURCES,
                       static_cast<uint64_t>(ea));
}

VOID AddMemWrite(THREADID tid, ADDRINT ea)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  WriteToSet<uint64_t>(st->curr.destination_memory,
                       st->curr.destination_memory + NUM_INSTR_DESTINATIONS,
                       static_cast<uint64_t>(ea));
}

// ---------- Instrumentation ----------
VOID Instruction(INS ins, VOID*)
{
  INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)ResetCurrentInstruction,
                 IARG_THREAD_ID, IARG_INST_PTR, IARG_END);

  if (INS_IsBranch(ins)) {
    INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)BranchOrNot,
                   IARG_THREAD_ID, IARG_BRANCH_TAKEN, IARG_END);
  }

  // // reg reads
  // const UINT32 rcount = INS_MaxNumRRegs(ins);
  // for (UINT32 i = 0; i < rcount; i++) {
  //   REG r = INS_RegR(ins, i);
  //   if (r != REG_INVALID()) {
  //     INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AddRegRead,
  //                    IARG_THREAD_ID, IARG_UINT32, (UINT32)r, IARG_END);
  //   }
  // }

  // // reg writes
  // const UINT32 wcount = INS_MaxNumWRegs(ins);
  // for (UINT32 i = 0; i < wcount; i++) {
  //   REG r = INS_RegW(ins, i);
  //   if (r != REG_INVALID()) {
  //     INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AddRegWrite,
  //                    IARG_THREAD_ID, IARG_UINT32, (UINT32)r, IARG_END);
  //   }
  // }

  // // mem operands
  // const UINT32 memOps = INS_MemoryOperandCount(ins);
  // for (UINT32 memOp = 0; memOp < memOps; memOp++) {
  //   if (INS_MemoryOperandIsRead(ins, memOp)) {
  //     INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AddMemRead,
  //                    IARG_THREAD_ID, IARG_MEMORYOP_EA, memOp, IARG_END);
  //   }
  //   if (INS_MemoryOperandIsWritten(ins, memOp)) {
  //     INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AddMemWrite,
  //                    IARG_THREAD_ID, IARG_MEMORYOP_EA, memOp, IARG_END);
  //   }
  // }

  INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR)ShouldWrite,
                   IARG_THREAD_ID, IARG_END);
  INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteCurrentInstruction,
                     IARG_THREAD_ID, IARG_END);
}

// ---------- Thread management ----------
VOID ThreadStart(THREADID tid, CONTEXT*, INT32, VOID*)
{
  auto* st = new thread_state_t();
  PIN_SetThreadData(tls_key, st, tid);

  PIN_GetLock(&lock, 1);
  if (tracked_tid == INVALID_THREADID) tracked_tid = tid; // track first thread only
  PIN_ReleaseLock(&lock);
}

VOID ThreadFini(THREADID tid, const CONTEXT*, INT32, VOID*)
{
  auto* st = GetState(tid);
  if (!st) return;

  if (tid == tracked_tid && g_out) {
    st->curr = {};
    st->curr.record_size = sizeof(trace_instr_format_t);
    st->curr.magic = END_MAGIC;
    st->curr.trace_window = 0;
    st->curr.window_id = window_id;

    fprintf(stderr, "instrCount=%lu\n", st->instrCount);
    fwrite(&st->curr, sizeof(trace_instr_format_t), 1, g_out);
    fflush(g_out);
    fclose(g_out);
    g_out = nullptr;
  }

  delete st;
  PIN_SetThreadData(tls_key, nullptr, tid);
}

INT32 Usage()
{
  fprintf(stderr, "Champsim tracer: -s <skip> -t <trace>\n");
  return -1;
}


static void open_trace_out()
{
  const std::string path = KnobOut.Value();
  g_out = fopen(path.c_str(), "wb");
  if (!g_out) {
  perror(("fopen " + path).c_str());
  PIN_ExitProcess(1);
  }
  fprintf(stderr,
        "g_out=%p fd=%d stdout_fd=%d\n",
        g_out,
        fileno(g_out),
        fileno(stdout));

  // // buffered is GOOD
  // setvbuf(g_out, nullptr, _IOFBF, 1 << 20); // 1MB buffer
}

extern "C" void signal_handler(int signum){
std::cerr << "Signal handler cntrl+c\n";
signal_received = signum;
}

VOID Fini(INT32 code, VOID* v)
{
  if (g_out) {
    trace_instr_format_t curr{};
    curr.record_size = sizeof(trace_instr_format_t);
    curr.magic = END_MAGIC;
    curr.trace_window = 0;
    curr.window_id = window_id;
    fwrite(&curr, sizeof(trace_instr_format_t), 1, g_out);
    fflush(g_out);
    fclose(g_out);
    g_out = nullptr;
  }
}

int main(int argc, char* argv[])
{
  if (PIN_Init(argc, argv)) return Usage();

  PIN_InitLock(&lock);
  tls_key = PIN_CreateThreadDataKey(nullptr);

  std::string phase_file_name = KnobPhaseFileInstructions.Value();
  phase_data = read_phase_csv(phase_file_name);
  open_trace_out();

  // buffered is GOOD
  setvbuf(g_out, nullptr, _IOFBF, 1 << 20); // 1MB buffer
  
  INS_AddInstrumentFunction(Instruction, nullptr);
  PIN_AddThreadStartFunction(ThreadStart, nullptr);
  PIN_AddThreadFiniFunction(ThreadFini, nullptr);
  PIN_AddFiniFunction(Fini, nullptr);

  PIN_StartProgram();
  return 0;
}
