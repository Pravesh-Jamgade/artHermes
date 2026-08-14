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
#define END_THFINI_MAGIC 0x1111425411114355ULL
#define END_FINI_MAGIC 0x1111425411114365ULL
static constexpr int NUM_INSTR_DESTINATIONS = 2;
static constexpr int NUM_INSTR_SOURCES      = 4;

int skip_initial_instructions = 0;
int trace_remaining_instructions = 0;
uint64_t window_id = 0;
int instr_id = 0;
volatile sig_atomic_t signal_received = 0;

char* re_excute[100] = {nullptr};
int j = 0;

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
KNOB<UINT64> KnobWindowSize(KNOB_MODE_WRITEONCE, "pintool", "window-size", "50000000", "trace");

// Declare the knob as a string type to accept decimal/fraction inputs
KNOB<std::string> KnobFraction(KNOB_MODE_WRITEONCE, "pintool", "skip-fraction", "0.5", "Fraction of window size to skip");
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
  // // const uint64_t start = KnobSkipInstructions.Value() + 1;
  // const uint64_t end   = KnobSkipInstructions.Value() + KnobTraceInstructions.Value();
  // const uint64_t window_size = KnobWindowSize.Value();
  // uint64_t new_window_id = (st->instrCount-1) / window_size;

  // // std::string fractionStr = KnobFraction.Value();
  // // double fractionValue = std::stod(fractionStr); 

  // // New window start
  // if(new_window_id != window_id){
  //   window_id = new_window_id;
  // }
  return TRUE;
}

VOID WriteCurrentInstruction(THREADID tid)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  st->curr.record_size = sizeof(trace_instr_format_t);//valid instruction
  st->curr.magic = st->instrCount > (KnobSkipInstructions.Value() + KnobTraceInstructions.Value()) ? END_MAGIC : MAGIC;
  st->curr.trace_window = 1;
  st->curr.window_id = window_id;

  /*write to buffer fifo*/
  if (!g_out) {
      fprintf(stderr, "g_out is NULL\n");
      PIN_ExitApplication(1);
  }

  fwrite(&st->curr, sizeof(trace_instr_format_t), 1, g_out);

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

  // reg reads
  const UINT32 rcount = INS_MaxNumRRegs(ins);
  for (UINT32 i = 0; i < rcount; i++) {
    REG r = INS_RegR(ins, i);
    if (r != REG_INVALID()) {
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AddRegRead,
                     IARG_THREAD_ID, IARG_UINT32, (UINT32)r, IARG_END);
    }
  }

  // reg writes
  const UINT32 wcount = INS_MaxNumWRegs(ins);
  for (UINT32 i = 0; i < wcount; i++) {
    REG r = INS_RegW(ins, i);
    if (r != REG_INVALID()) {
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AddRegWrite,
                     IARG_THREAD_ID, IARG_UINT32, (UINT32)r, IARG_END);
    }
  }

  // mem operands
  const UINT32 memOps = INS_MemoryOperandCount(ins);
  for (UINT32 memOp = 0; memOp < memOps; memOp++) {
    if (INS_MemoryOperandIsRead(ins, memOp)) {
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AddMemRead,
                     IARG_THREAD_ID, IARG_MEMORYOP_EA, memOp, IARG_END);
    }
    if (INS_MemoryOperandIsWritten(ins, memOp)) {
      INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)AddMemWrite,
                     IARG_THREAD_ID, IARG_MEMORYOP_EA, memOp, IARG_END);
    }
  }

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
    st->curr.magic = END_THFINI_MAGIC;
    st->curr.trace_window = 0;
    st->curr.window_id = window_id;

    fprintf(stderr, "ThreadFini: instrCount=%lu, Tracked Thraed Id=%d\n", st->instrCount, tid);
    fwrite(&st->curr, sizeof(trace_instr_format_t), 1, g_out);
    fflush(g_out);
    fclose(g_out);
    g_out = nullptr;

    
    pid_t pid = fork();

    if (pid < 0) {
        // Fork failed
        std::cerr << "Fork failed!" << std::endl;
        _exit(1); 
    } 
    else if (pid == 0) {

        fprintf(stderr, "Re-executing the application with execv\n");
        for(int k=0; k<=j; k++)
        {
          fprintf(stderr, "re_excute[%d] = %s \n", k, re_excute[k]);
        }
        execv(re_excute[0], re_excute);
        fprintf(stderr, "execv failed with errno=%d\n", errno);

        // --- CHILD PROCESS ---
        // Detach from the terminal session group so it runs cleanly
        setsid(); 
        
        // Now execute your clean array
        execv(re_excute[0], re_excute);
        
        // If execv fails inside the child, kill it immediately
        _exit(127); 
    }
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
    curr.magic = END_FINI_MAGIC;
    curr.trace_window = 0;
    curr.window_id = window_id;
    // fprintf(stderr, "Fini: instrCount=%lu\n", st->instrCount);
    fwrite(&curr, sizeof(trace_instr_format_t), 1, g_out);
    fflush(g_out);
    fclose(g_out);
    g_out = nullptr;

    fprintf(stderr, "From FINI: Re-executing the application with execv\n");
    execv(re_excute[0], re_excute);
  }
}

int main(int argc, char* argv[])
{
  if (PIN_Init(argc, argv)) return Usage();

  for(int i=0; i< 100; i++)
  {
    re_excute[i] = (char*)malloc(100*sizeof(char));
  }
  
  bool record = false;
  re_excute[j++] = (char*) "/home/pravesh/pin_kit/pin";
  for(int i=0; i< argc; i++)
  {
    fprintf(stderr, "argv[%d] = %s \n", i, argv[i]);
    if(strcmp(argv[i], "-t") == 0){
      record = true;
    }
    if(record){
      re_excute[j++] = argv[i];
    }
  }
  re_excute[j] = nullptr;

  PIN_InitLock(&lock);
  tls_key = PIN_CreateThreadDataKey(nullptr);

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
