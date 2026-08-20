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
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef SYS_gettimeofday
#define SYS_gettimeofday __NR_gettimeofday
#endif
#ifndef SYS_clock_gettime
#define SYS_clock_gettime __NR_clock_gettime
#endif

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
  bool in_spinlock = false;
  // std::ofstream outfile;
};

static TLS_KEY tls_key;
static PIN_LOCK lock;
static THREADID tracked_tid = INVALID_THREADID;
static uint64_t total_spin_locks_encountered = 0;

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

  if (st->in_spinlock) return FALSE;

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

  uint64_t total_limit = KnobSkipInstructions.Value() + KnobTraceInstructions.Value();
  if (st->instrCount > total_limit + 1) {
    return;
  }

  st->curr.record_size = sizeof(trace_instr_format_t);//valid instruction
  st->curr.magic = st->instrCount > total_limit ? END_MAGIC : MAGIC;
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

VOID OnSpinLockEnter(THREADID tid)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (st) {
    st->in_spinlock = true;
    total_spin_locks_encountered++;
  }
}

VOID OnSpinLockExit(THREADID tid)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (st) {
    st->in_spinlock = false;
  }
}

VOID Routine(RTN rtn, VOID* v)
{
  std::string name = RTN_Name(rtn);
  if (name == "pthread_spin_lock" || name == "__pthread_spin_lock" || name == "_pthread_spin_lock")
  {
    RTN_Open(rtn);
    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)OnSpinLockEnter,
                   IARG_THREAD_ID, IARG_END);
    RTN_Close(rtn);
  }
  else if (name == "pthread_spin_unlock" || name == "__pthread_spin_unlock" || name == "_pthread_spin_unlock")
  {
    RTN_Open(rtn);
    RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)OnSpinLockExit,
                   IARG_THREAD_ID, IARG_END);
    RTN_Close(rtn);
  }
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

// Structure to store syscall arguments at entry, so they can be modified at exit
struct syscall_info_t {
  ADDRINT num;
  ADDRINT arg0;
  ADDRINT arg1;
};

// Thread-local array to save syscall details per thread (supporting up to 1024 threads)
static syscall_info_t sys_info[1024];

// Captures system call number and arguments at entry
VOID SyscallEntry(THREADID threadIndex, CONTEXT *ctxt, SYSCALL_STANDARD std, VOID *v)
{
  if (threadIndex != tracked_tid) return;
  ADDRINT syscall_number = PIN_GetSyscallNumber(ctxt, std);
  sys_info[threadIndex].num = syscall_number;
  sys_info[threadIndex].arg0 = PIN_GetSyscallArgument(ctxt, std, 0);
  sys_info[threadIndex].arg1 = PIN_GetSyscallArgument(ctxt, std, 1);
}

// Overrides time-related system calls at exit to return deterministic virtual time
VOID SyscallExit(THREADID threadIndex, CONTEXT *ctxt, SYSCALL_STANDARD std, VOID *v)
{
  if (threadIndex != tracked_tid) return;
  auto* st = GetState(threadIndex);
  if (!st) return;

  ADDRINT syscall_number = sys_info[threadIndex].num;
  
  // Virtualize gettimeofday
  if (syscall_number == SYS_gettimeofday) {
    ADDRINT arg0 = sys_info[threadIndex].arg0;
    if (arg0 != 0) {
      struct timeval tv;
      uint64_t insts = st->instrCount;
      // Virtual time: 0.5 ns per instruction (matching 2 GHz core) from a fixed start epoch
      uint64_t total_ns = insts * 0.5;
      tv.tv_sec = 1700000000 + (total_ns / 1000000000ULL);
      tv.tv_usec = (total_ns % 1000000000ULL) / 1000ULL;
      PIN_SafeCopy(reinterpret_cast<void*>(arg0), &tv, sizeof(struct timeval));
    }
  }
  // Virtualize clock_gettime
  else if (syscall_number == SYS_clock_gettime) {
    ADDRINT arg1 = sys_info[threadIndex].arg1;
    if (arg1 != 0) {
      struct timespec ts;
      uint64_t insts = st->instrCount;
      // Virtual time: 0.5 ns per instruction (matching 2 GHz core) from a fixed start epoch
      uint64_t total_ns = insts * 0.5;
      ts.tv_sec = 1700000000 + (total_ns / 1000000000ULL);
      ts.tv_nsec = total_ns % 1000000000ULL;
      PIN_SafeCopy(reinterpret_cast<void*>(arg1), &ts, sizeof(struct timespec));
    }
  }
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

    fprintf(stderr, "ThreadFini: instrCount=%lu, Tracked Thraed Id=%d, total_spin_locks_encountered=%lu\n", st->instrCount, tid, total_spin_locks_encountered);
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
  fprintf(stderr, "Fini: total_spin_locks_encountered=%lu\n", total_spin_locks_encountered);
  if (g_out) {
    trace_instr_format_t curr{};
    curr.record_size = sizeof(trace_instr_format_t);
    curr.magic = END_FINI_MAGIC;
    curr.trace_window = 0;
    curr.window_id = window_id;
    // fprintf(stderr, "Fini: instrCount=%lu\n", ->instrCount);
    fwrite(&curr, sizeof(trace_instr_format_t), 1, g_out);
    fflush(g_out);
    fclose(g_out);
    g_out = nullptr;
  }
}

int main(int argc, char* argv[])
{
  PIN_InitSymbols();
  if (PIN_Init(argc, argv)) return Usage();

  PIN_InitLock(&lock);
  tls_key = PIN_CreateThreadDataKey(nullptr);

  open_trace_out();

  // buffered is GOOD
  setvbuf(g_out, nullptr, _IOFBF, 1 << 20); // 1MB buffer
  
  INS_AddInstrumentFunction(Instruction, nullptr);
  RTN_AddInstrumentFunction(Routine, nullptr);
  PIN_AddSyscallEntryFunction(SyscallEntry, nullptr);
  PIN_AddSyscallExitFunction(SyscallExit, nullptr);
  PIN_AddThreadStartFunction(ThreadStart, nullptr);
  PIN_AddThreadFiniFunction(ThreadFini, nullptr);
  PIN_AddFiniFunction(Fini, nullptr);

  PIN_StartProgram();
  return 0;
}
