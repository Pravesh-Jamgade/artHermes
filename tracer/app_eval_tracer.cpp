#include "pin.H"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <set>
#include "app_eval_helper.h"

/*
phase_detect.cpp
*/

#define MAGIC 0x544C425452414345ULL
static constexpr int NUM_INSTR_DESTINATIONS = 2;
static constexpr int NUM_INSTR_SOURCES      = 4;

int instr_id = 0;
uint64_t instruction_count = 0;

// uint32_t id = 0;// 1 for store , 2 for load
struct context_instr {

  uint64_t destination_memory[NUM_INSTR_DESTINATIONS] = {};
  uint64_t source_memory[NUM_INSTR_SOURCES] = {};
  uint64_t ip = 0;
  uint64_t magic = 0;
  uint32_t record_size = 0;// 1 for store , 2 for load
  uint8_t is_branch = 0;
  uint8_t branch_taken = 0;
  uint8_t destination_registers[NUM_INSTR_DESTINATIONS] = {};
  uint8_t source_registers[NUM_INSTR_SOURCES] = {};
};

using trace_instr_format_t = context_instr;

static TLS_KEY tls_key;
static PIN_LOCK lock;
static THREADID tracked_tid = INVALID_THREADID;

KNOB<UINT64> KnobSkipInstructions (KNOB_MODE_WRITEONCE, "pintool", "s", "0", "skip");
KNOB<UINT64> KnobTraceInstructions(KNOB_MODE_WRITEONCE, "pintool", "t", "1000000", "trace");
KNOB<UINT64> KnobWindowInst(KNOB_MODE_WRITEONCE, "pintool", "w", "10000000", "window instructions (for PPKI)");
KNOB<std::string> KnobPhaseOut(KNOB_MODE_WRITEONCE, "pintool", "phase_out", "ppki.csv", "PPKI CSV output file");

// ---------- helper: insert unique into fixed array ----------

 // Convert integer to hex string
 inline std::string intToHex(uint64_t value) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase << value;
  return oss.str();
}

struct thread_state_t {
  uint64_t instrCount = 0;
  trace_instr_format_t curr{};

  // windowing
  uint64_t win_instr = 0;
  uint64_t win_id = 0;

  std::set<uint64_t> pages4k;
  std::set<uint64_t> prev_pages4k;
  
  SetAssocTLB tlb;
  FILE* phase_fp = nullptr;

  thread_state_t() : tlb(128, 16, 4096) {}
};

static inline uint64_t vpn4k(uint64_t addr) { return addr >> 12; }

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


static inline void MaybeEndWindow(thread_state_t* st, THREADID tid)
{
  const uint64_t W = KnobWindowInst.Value();
  if (W == 0) return;

  if (st->win_instr < W) return;

  // compute metrics
  const uint64_t insts = st->win_instr;
  const uint64_t uniq4k = st->pages4k.size();
  
  // churn (new pages vs prev window)
  uint64_t new4k = 0;
  uint64_t intersect = 0;
  if (!st->prev_pages4k.empty()) {
    for (auto p : st->pages4k)
      if (st->prev_pages4k.find(p) == st->prev_pages4k.end()) new4k++;
      else intersect++;
  } else {
    new4k = uniq4k; // first window, all new
  }

  // PPKI in this window
  double ppki4k = 0.0;
  if (insts) ppki4k = (double)uniq4k * 1000.0 / (double)insts;

  // New pages fraction in this window
  double new4k_frac_prev4k = (double)new4k/(double)uniq4k;

  // New pages found in this window w.r.t prev window in KI 
  double newpageski = 0.0;
  if (insts) newpageski = (double)new4k * 1000.0 / (double)insts;

  // jaccard
  double jac = 0.0;
  jac = (double)intersect/(st->prev_pages4k.size() + uniq4k - intersect);

  // SetAssocTLB::Stats stat_obj = st->tlb.stats();
  // uint64_t total_misses = stat_obj.misses;

  // log (file, NOT stdout)
  if (st->phase_fp) {
    fprintf(st->phase_fp, "%u,%llu,%llu,%llu,%.6f,%llu,%.6f,%.6f,%.6f\n",
            (unsigned)tid,
            (unsigned long long)st->win_id,
            (unsigned long long)insts,
            (unsigned long long)uniq4k,
            ppki4k,
            (unsigned long long)new4k,
            newpageski,
            new4k_frac_prev4k,
            jac);
    fflush(st->phase_fp);
  }

  // roll window
  st->prev_pages4k.swap(st->pages4k);
  st->pages4k.clear();
  st->win_instr = 0;
  st->win_id++;
}

BOOL ShouldWrite(THREADID tid)
{
  if (tid != tracked_tid) return FALSE;
  auto* st = GetState(tid);
  if (!st) return FALSE;

  ++st->instrCount;
  const uint64_t n = st->instrCount;
  const uint64_t start = KnobSkipInstructions.Value() + 1;
  const uint64_t end   = KnobSkipInstructions.Value() + KnobTraceInstructions.Value();

  st->win_instr++;
  MaybeEndWindow(st, tid);

  if (n > end) {
    PIN_ExitApplication(0);
    return FALSE;
  }

  return (n >= start && n <= end) ? TRUE : FALSE;
}

VOID WriteCurrentInstruction(THREADID tid)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

}

// Memory helpers (ADDRINT / 64-bit)
VOID AddMemRead(THREADID tid, ADDRINT ea)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  st->tlb.access(ea);

  WriteToSet<uint64_t>(st->curr.source_memory,
                       st->curr.source_memory + NUM_INSTR_SOURCES,
                       static_cast<uint64_t>(ea));
  st->pages4k.insert(vpn4k((uint64_t)ea));
}

VOID AddMemWrite(THREADID tid, ADDRINT ea)
{
  if (tid != tracked_tid) return;
  auto* st = GetState(tid);
  if (!st) return;

  st->tlb.access(ea);

  WriteToSet<uint64_t>(st->curr.destination_memory,
                       st->curr.destination_memory + NUM_INSTR_DESTINATIONS,
                       static_cast<uint64_t>(ea));
  
  st->pages4k.insert(vpn4k((uint64_t)ea));
}

// ---------- Instrumentation ----------
VOID Instruction(INS ins, VOID*)
{
  INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)ResetCurrentInstruction,
                 IARG_THREAD_ID, IARG_INST_PTR, IARG_END);

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

  if (tid == tracked_tid) {
    st->tlb = SetAssocTLB(128, 16, 4096);
    st->phase_fp = fopen(KnobPhaseOut.Value().c_str(), "w");
    if (st->phase_fp) {
      fprintf(st->phase_fp, "tid,win_id,insts,uniq4k,ppki4k,new4k,newpageski,new_frac,jac\n");
      fflush(st->phase_fp);
    }
  }
}

VOID ThreadFini(THREADID tid, const CONTEXT*, INT32, VOID*)
{
  auto* st = GetState(tid);
  // if(tid == tracked_tid)
  //   st->outfile.close();
  delete st;
  PIN_SetThreadData(tls_key, nullptr, tid);
}

INT32 Usage()
{
  fprintf(stderr, "Champsim tracer: -s <skip> -t <trace>\n");
  return -1;
}

int main(int argc, char* argv[])
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (PIN_Init(argc, argv)) return Usage();

  PIN_InitLock(&lock);
  tls_key = PIN_CreateThreadDataKey(nullptr);

  INS_AddInstrumentFunction(Instruction, nullptr);
  PIN_AddThreadStartFunction(ThreadStart, nullptr);
  PIN_AddThreadFiniFunction(ThreadFini, nullptr);

  PIN_StartProgram();
  return 0;
}
