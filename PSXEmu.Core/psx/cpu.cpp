/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#include "psx/psx.h"

bool output_inst = false;
uint32_t until_address =0;
#define CSVOUT
#define CPU_DEBUG
//#define BIOSCALL

namespace emulation {
namespace psx {

Buffer* ICache2::GetBufferAndOffset(uint32_t address, uint32_t& output_offset) {
  uint32_t pc_bank, pc_offset, pc_cache;
  pc_bank = address >> 24;
  pc_offset = address & 0xffffff;
  pc_cache = address & 0xfff;
  
  if ((system_->io().io.cache_control & 0x800) == 0) {
    if (pc_bank == 0xA0 || pc_bank == 0x80|| pc_bank == 0x00) {
      output_offset = address & 0x001FFFFF;
      return &system_->io().ram_buffer;
    } else if (pc_bank == 0x1F || pc_bank == 0x9F|| pc_bank == 0xBF) {
      output_offset = address & 0x0007FFFF;
      return &system_->io().bios_buffer;
    } else {
      return nullptr;
    }
  }

  if (pc_bank == 0xA0) {//non cache segments
    output_offset = address & 0x001FFFFF;
    return &system_->io().ram_buffer;
  }
  if (pc_bank == 0xBF) {//non cache segments
    output_offset = address & 0x0007FFFF;
    return &system_->io().bios_buffer;
  }
  if (addresses[pc_cache] == pc_offset) {
    output_offset = pc_cache;
    return &buffer;
  } else {
    addresses[pc_cache] = pc_offset;
    buffer.u32[pc_cache>>2] = system_->io().ram_buffer.u32[(address & 0x001FFFFF)>>2];
    output_offset = pc_cache;
    return &buffer;
  }
  
    
}

Cpu::Instruction Cpu::machine_instruction_main_[64] = {
  &Cpu::SPECIAL, &Cpu::REGIMM , &Cpu::J      , &Cpu::JAL    , &Cpu::BEQ    , &Cpu::BNE    , &Cpu::BLEZ   , &Cpu::BGTZ   ,
  &Cpu::ADDI   , &Cpu::ADDIU  , &Cpu::SLTI   , &Cpu::SLTIU  , &Cpu::ANDI   , &Cpu::ORI    , &Cpu::XORI   , &Cpu::LUI    ,
  &Cpu::COP0   , &Cpu::UNKNOWN, &Cpu::COP2   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::LB     , &Cpu::LH     , &Cpu::LWL    , &Cpu::LW     , &Cpu::LBU    , &Cpu::LHU    , &Cpu::LWR    , &Cpu::UNKNOWN,
  &Cpu::SB     , &Cpu::SH     , &Cpu::SWL    , &Cpu::SW     , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::SWR    , &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::LWC2   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::SWC2   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
};

Cpu::Instruction Cpu::machine_instruction_special_[64] = {
  &Cpu::SLL    , &Cpu::UNKNOWN, &Cpu::SRL    , &Cpu::SRA    , &Cpu::SLLV   , &Cpu::UNKNOWN, &Cpu::SRLV   , &Cpu::SRAV   ,
  &Cpu::JR     , &Cpu::JALR   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::SYSCALL, &Cpu::BREAK  , &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::MFHI   , &Cpu::MTHI   , &Cpu::MFLO   , &Cpu::MTLO   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::MULT   , &Cpu::MULTU  , &Cpu::DIV    , &Cpu::DIVU   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::ADD    , &Cpu::ADDU   , &Cpu::SUB    , &Cpu::SUBU   , &Cpu::AND    , &Cpu::OR     , &Cpu::XOR    , &Cpu::NOR    ,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::SLT    , &Cpu::SLTU   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN
};

Cpu::Instruction Cpu::machine_instruction_regimm_[32] = {
  &Cpu::BLTZ   , &Cpu::BGEZ   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::BLTZAL , &Cpu::BGEZAL , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN
};

/******************************************************************************
* Name        : Cpu
* Description : Cpu Constructor
* Parameters  : (none)
*
* Notes :
* 
* 
*******************************************************************************/
Cpu::Cpu() : __inside_instruction(false),__inside_delay_slot(false), context_(NULL) {
  memset(bios_logged,0,sizeof(bios_logged));
  /*DecodeIType dit;
  decoders[0] = dit;
  DecodeJType djt;
  decoders[1] = djt;
  DecodeRType drt;
  decoders[2] = drt;
  */
}

/******************************************************************************
* Name        : ~Cpu
* Description : Cpu Destructor
* Parameters  : (none)
*
* Notes :
* 
* 
*******************************************************************************/
Cpu::~Cpu() {
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(BIOSCALL)
    bioscode.Close();
  #endif
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    system_->csvlog.Close();
  #endif
}


int Cpu::Initialize() {
  cpu_ = this;
  icache.set_system(system_);
  icache.Initialize();
  InvalidateICacheTags();
  //dcache_.Initialize();

  index = 0;
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    system_->csvlog.OutputCSVHeader();
  #endif
  current_stage = 0;
  context_->gp.zero = 0;
  context_->ctrl.PRId = 0x00000002;
  //context_->ctrl.PRId = 3 << 8; //R3000A
  context_->ctrl.SR.raw = 0x10900000;//1111
  context_->cycles = 0;
  context_->current_cycles = 0;
  // Nothing is in flight on a cold start, and leaving a stale record here
  // would write a register during the first instruction after a reset.
  pending_load_ = PendingLoad();
  armed_load_ = PendingLoad();
  return 0;
}

void Cpu::InvalidateICacheTags() {
  if (!icache_tags_)
    return;
  for (int i = 0; i < kICacheLines; ++i)
    icache_tags_[i] = 0xFFFFFFFFu;
}

// The model belongs to the interpreter. With the recompiler on it is kept off
// rather than left to see only the odd interpreted step - a GTE command, an
// interrupt entry - with tags that describe nothing.
void Cpu::SyncICacheSetting() {
  const EmuConfig& config = system_->config();
  const bool on = config.icache_timing && !system_->recompiler_enabled();
  if (on != icache_timing_)
    set_icache_timing(on);

  // The two bus models, latched here for the same reason. The write queue is the
  // interpreter's alone: compiled code charges its cycles after a block, so every
  // store in one would see the same clock and the queue would never drain.
  const bool measured = config.measured_bus_timing;
  const bool queue = config.write_queue_timing && !system_->recompiler_enabled();
  if (measured != measured_bus_ || queue != write_queue_) {
    if ((measured || queue) && !bus_model_)
      bus_model_ = std::make_unique<BusModel>();
    if (!queue && bus_model_)
      bus_model_->count = 0;
    measured_bus_ = measured;
    write_queue_ = queue;
  }
}

// A load from an 8- or 16-bit region: psx-spx's formula by default, and with
// measured_bus_timing the rule fitted to a console (IOInterface::
// MeasuredAccessCycles), which also knows whether the bus was just busy and reads
// only the halfwords or bytes an lwl or lwr needs.
uint32_t Cpu::NarrowLoadStall(int region, uint32_t width) {
  const IOInterface& io = system_->io();
  const auto bus_region = static_cast<IOInterface::BusRegion>(region);
  if (!measured_bus_)
    return io.bus_stall(bus_region, width);

  const IOInterface::BusCost& cost = io.bus_read_[region];
  uint32_t units;
  if (partial_bytes_ != 0)
    units = IOInterface::BusUnits(cost, partial_lane_, partial_bytes_);
  else
    units = IOInterface::BusUnits(cost, 0, 1u << width);
  const uint64_t now = context_->cycles;
  const uint64_t gap = now > bus_model_->idle_since ? now - bus_model_->idle_since : 0;
  const uint32_t total = IOInterface::MeasuredAccessCycles(cost, units, gap);
  const uint32_t stall = total > 1 ? total - 1 : 0;
  bus_model_->idle_since = now + stall;
  return stall;
}

// Where a queued store's drain time comes from: the same access read would take.
// RAM 5 and the on-die registers 3, as loads measure; the narrow regions from their
// write delay, by whichever bus rule is in force. Nothing measured says any of this
// for writes - see EmuConfig::write_queue_timing.
uint32_t Cpu::StoreOccupancy(uint32_t physical, MemorySize size) {
  const IOInterface& io = system_->io();
  int region = -1;
  if (physical <= 0x007FFFFF)                                    return 5;
  else if (physical >= 0x1F801800 && physical <= 0x1F80180F)     region = IOInterface::kBusCdrom;
  else if (physical >= 0x1F801C00 && physical <= 0x1F801FFF)     region = IOInterface::kBusSpu;
  else if (physical >= 0x1F802000 && physical <= 0x1F802FFF)     region = IOInterface::kBusExp2;
  else if (physical >= 0x1F801000 && physical <= 0x1F801FFF)     return 3;
  else if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF)     region = IOInterface::kBusBios;
  else if (physical >= 0x1F000000 && physical <= 0x1F7FFFFF)     region = IOInterface::kBusExp1;
  else if (physical >= 0x1FA00000 && physical <= 0x1FBFFFFF)     region = IOInterface::kBusExp3;
  else                                                           return 7;
  const IOInterface::BusCost& cost =
      measured_bus_ ? io.bus_write_[region] : io.bus_write_formula_[region];
  const uint32_t units = IOInterface::BusUnits(cost, 0, static_cast<uint32_t>(size));
  return cost.first + (units - 1) * cost.seq;
}

// psx-spx: "Store operations are passed to the write-queue, so they can execute
// within a single clock cycle (unless the write-queue was full, in which case the CPU
// gets halted until there's room in the queue)." The R3000A's queue is four deep.
// Each entry holds the bus for its occupancy, one after another.
void Cpu::QueueStore(uint32_t occupancy) {
  BusModel& bus = *bus_model_;
  uint64_t now = context_->cycles;
  while (bus.count > 0 && bus.done[bus.head] <= now) {
    bus.head = (bus.head + 1) % BusModel::kDepth;
    --bus.count;
  }
  if (bus.count == BusModel::kDepth) {
    const uint64_t wait = bus.done[bus.head] - now;
    for (uint64_t i = 0; i < wait; ++i)
      Tick();
    now = context_->cycles;
    bus.head = (bus.head + 1) % BusModel::kDepth;
    --bus.count;
  }
  const uint64_t start = bus.last_done > now ? bus.last_done : now;
  bus.last_done = start + occupancy;
  bus.done[(bus.head + bus.count) % BusModel::kDepth] = bus.last_done;
  ++bus.count;
}

// A load needs the bus, and the queued stores have it first: the load waits until
// the last of them is done.
void Cpu::DrainWriteQueue() {
  BusModel& bus = *bus_model_;
  if (bus.count == 0)
    return;
  const uint64_t now = context_->cycles;
  if (bus.last_done > now) {
    const uint64_t wait = bus.last_done - now;
    for (uint64_t i = 0; i < wait; ++i)
      Tick();
  }
  bus.count = 0;
  bus.head = 0;
}

void Cpu::set_icache_timing(bool on) {

  // Switching it on starts from a cold cache: tags left from before it was off
  // describe code that may no longer be where they say.
  if (on && !icache_timing_) {
    if (!icache_tags_)
      icache_tags_ = std::make_unique<uint32_t[]>(kICacheLines);
    InvalidateICacheTags();
  }
  icache_timing_ = on;
}

// The fetch cost, on DuckStation's model with this core's own bus costs.
//
// KUSEG and KSEG0 go through the cache. A hit costs nothing beyond the
// instruction's own cycle. A miss refills from the word being fetched to the end
// of its line: one cycle a word from RAM, which the cache reads in a burst, and
// the full access for every word from the BIOS ROM or expansion 1, which sit on
// 8-bit buses and have no burst.
//
// KSEG1 is uncached, and every fetch is a single bus read costing what a load
// from the same place costs - 4 from RAM, the ROM's programmed delay (about 24 a
// word) from the BIOS. Those are the figures JaCzekanski's access-time test
// measured for loads (Cpu::Load); DuckStation charges 6 for RAM here, but the bus
// does not know whether it is fetching or loading.
//
// The cache-enable bit in 0xFFFE0130 is not consulted: DuckStation does not
// either, and the BIOS turns the cache on before anything runs from a cached
// segment.
uint32_t Cpu::FetchStall(uint32_t pc) {
  const uint32_t segment = pc >> 29;
  const uint32_t physical = pc & 0x1FFFFFFF;
  const IOInterface& io = system_->io();

  if (segment == 0 || segment == 4) {
    const uint32_t line = (pc >> 4) & (kICacheLines - 1);
    const uint32_t word = (pc >> 2) & 3;
    const uint32_t tag = pc & 0xFFFFFFF0u;
    if ((icache_tags_[line] & (0xFFFFFFF0u | (1u << word))) == tag) {
      ++icache_hits_;
      return 0;
    }
    ++icache_misses_;
    static const uint32_t kAbsentBefore[4] = { 0, 1, 3, 7 };
    icache_tags_[line] = tag | kAbsentBefore[word];
    const uint32_t words = 4 - word;
    if (physical <= 0x007FFFFF)
      return words;
    if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF)
      return words * (io.bus_stall(IOInterface::kBusBios, 2) + 1);
    if (physical >= 0x1F000000 && physical <= 0x1F7FFFFF)
      return words * (io.bus_stall(IOInterface::kBusExp1, 2) + 1);
    return 0;
  }

  if (segment == 5) {
    ++uncached_fetches_;
    if (physical <= 0x007FFFFF)
      return 4;
    if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF)
      return io.bus_stall(IOInterface::kBusBios, 2);
    if (physical >= 0x1F000000 && physical <= 0x1F7FFFFF)
      return io.bus_stall(IOInterface::kBusExp1, 2);
    return 0;
  }
  return 0;
}

int Cpu::Deinitialize() {

//  dcache_.Deinitialize();
  icache.Deinitialize();
  return 0;
}

void Cpu::Serialise(StateIO& io) {
  if (!io.saving()) {
    InvalidateICacheTags();
    // Timing state, not machine state: a loaded state starts with the write queue
    // empty and the bus idle, and a DMA stall cannot be pending between steps.
    if (bus_model_)
      *bus_model_ = BusModel();
    dma_stall_cycles_ = 0;
  }
  io.Bytes(icache.buffer.u8, 0x1000 * 4);
  io.Plain(icache.addresses);
  io.Plain(pending_load_);
  io.Plain(armed_load_);
  io.Plain(gte_busy_until_cycles_);
  io.Plain(hilo_busy_until_cycles_);
}

// The debugger's side of Load and Store, kept out of line: the call and its
// arguments inside those two - the hottest functions in the interpreter -
// cost about 3% of the BIOS boot with no watchpoint set, by changing how the
// compiler laid out the rest of them. Behind a never-inlined call the flag
// test is all that is left there.
__declspec(noinline) void Cpu::WatchLoad(MemorySize size, uint32_t address) {
  if (current_stage == 1 || merging_store_)
    return;
  const uint32_t bytes = (size == kM8) ? 1u : (size == kM16) ? 2u : 4u;
  system_->debugger().OnCpuAccess(context_->prev_pc, address, bytes, false, 0);
}

__declspec(noinline) void Cpu::WatchStore(MemorySize size, uint32_t address, uint32_t data) {
  const uint32_t bytes = (size == kM8) ? 1u : (size == kM16) ? 2u : 4u;
  system_->debugger().OnCpuAccess(context_->prev_pc, address, bytes, true, data);
}

void Cpu::NoteExternalWrite(uint32_t tag, uint32_t byte_address,
                            uint32_t value) {
  // The DMA channels tag their writes D0h plus the channel.
  if (debug_watch_ && (tag & 0xF0) == 0xD0)
    system_->debugger().OnDmaWrite(static_cast<int>(tag & 0x0F), byte_address, value);
  if (watch_address_ == 0)
    return;
  const uint32_t low = byte_address & 0x1FFFFF;
  const uint32_t target = watch_address_ & 0x1FFFFF;
  if (low + 4 <= target || low >= target + 4)
    return;
  Watch& w = watch_[watch_count_ % kWatchCapacity];
  w.pc = tag;
  w.address = byte_address;
  w.value = value;
  w.size = 4;
  ++watch_count_;
}

void Cpu::DumpTrace(const char* filename, ExceptionCodes code) {
  FILE* fp = fopen(filename, "w");
  if (!fp) return;

  fprintf(fp, "--- PSX Execution Trace ---\n");
  for (int i = 0; i < kTraceSize; ++i) {
    int idx = (trace_index_ + i) % kTraceSize;
    if (trace_buffer_[idx].pc == 0) continue; // Skip uninitialized entries

    uint32_t pc = trace_buffer_[idx].pc;
    uint32_t inst = trace_buffer_[idx].instruction;
    
    // Simple hex dump for now, decoding requires more setup, 
    // but the PC and Instruction hex are enough to trace loops.
    fprintf(fp, "[%05d] PC: 0x%08X  Inst: 0x%08X\n", i, pc, inst);
  }
  
  fprintf(fp, "--- Exception State ---\n");
  fprintf(fp, "Caught Code: 0x%02X\n", code);
  fprintf(fp, "EPC: 0x%08X\n", context_->ctrl.EPC);
  fprintf(fp, "Cause: 0x%08X\n", context_->ctrl.Cause);
  fprintf(fp, "BadVaddr: 0x%08X\n", context_->ctrl.BadVaddr);
  fprintf(fp, "a0: 0x%08X\n", context_->gp.reg[4]);
  fprintf(fp, "a1: 0x%08X\n", context_->gp.reg[5]);
  fprintf(fp, "a2: 0x%08X\n", context_->gp.reg[6]);
  fprintf(fp, "a3: 0x%08X\n", context_->gp.reg[7]);
  fprintf(fp, "v0: 0x%08X\n", context_->gp.reg[2]);
  fprintf(fp, "v1: 0x%08X\n", context_->gp.reg[3]);
  fprintf(fp, "ra: 0x%08X\n", context_->gp.reg[31]);

  fprintf(fp, "--- End of Trace ---\n");
  fclose(fp);
}


// Moves the load pipeline on by one instruction.
//
// Both halves of the load delay are here. The value a load promised lands in
// the register file at the start of the second instruction after it, so the
// one in between reads whatever the register held before - which is the whole
// point, and what software written for this CPU expects.
//
// Doing this at the start of an instruction rather than at the end is not a
// detail. A branch runs its delay slot as a nested ExecuteInstruction, so
// end-of-instruction bookkeeping would run the slot's before the branch's,
// out of program order, and a load two instructions before a branch would
// reach its register one instruction late.
void Cpu::AdvanceLoadDelay() {
  if (pending_load_.active) {
    if (pending_load_.reg != 0)
      context_->gp.reg[pending_load_.reg] = pending_load_.value;
    pending_load_.active = false;
  }
  if (armed_load_.active) {
    pending_load_ = armed_load_;
    armed_load_.active = false;
  }
}
void Cpu::ExecuteInstruction() {
  context_->prev_pc = context_->pc;
  AdvanceLoadDelay();
  context_->gp.zero = 0; //make sure r0 is always 0.
  
  /*if (context_->pc == 0x000000A0 && context_->gp.t1 == 0x40) {
    // Automatically dump trace when SystemError A0(0x40) is hit
    DumpTrace("trace_crash.txt");
    // To prevent infinite dumping, change t1 so it doesn't match again immediately
    context_->gp.t1 = 0; 
  }*/

  StageIF();
  
  //if trace required, record instruction in ring buffer
  //if (1 == 0)
  {
      // Record instruction in ring buffer
      trace_buffer_[trace_index_].pc = context_->prev_pc;
      trace_buffer_[trace_index_].instruction = context_->code;
      trace_index_ = (trace_index_ + 1) % kTraceSize;
  }

  StageRD();
  current_stage = 3;
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    if (output_inst == true) {
      system_->csvlog.OutputInstruction2();
    }
  #endif
  index++;
  __inside_instruction = true;
  (this->*(machine_instruction_main_[opcode_]))();
  __inside_instruction = false;
  // A BIOS call arriving at A0h/B0h/C0h is noticed by System::StepInstruction,
  // before the instruction there runs - not here, where compiled code would
  // never pass.
}

// Is the instruction the pc is sitting on a GTE command? Peeked rather than
// fetched: this runs before the instruction is executed, and a fetch here
// would charge cycles and could raise a fault of its own.
//
// The test is the one the BIOS exception handler makes: COP2 with bit 25 set,
// which is 0x4A or 0x4B in the top byte. See System::StepInstruction for why
// an interrupt must not be taken in front of one.
bool Cpu::NextIsGteCommand() {
  // Masked down the way AddressTranslation does, but without it: that function
  // also sets the cache and bus-error flags, and Load() reads the bus-error
  // flag left by the previous translation. Disturbing it here would change
  // what the fetch immediately after this sees. KSEG2 falls outside both
  // ranges below and answers false, which is right - nothing executes there.
  const uint32_t pc = context_->pc;
  const uint32_t physical = (pc < 0x80000000) ? pc : (pc & 0x1FFFFFFF);
  uint32_t word;
  if (physical <= 0x007FFFFF)
    word = system_->io().ram_buffer.u32[(physical & 0x001FFFFF) >> 2];
  else if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF)
    word = system_->io().bios_buffer.u32[(physical & 0x0007FFFF) >> 2];
  else
    return false;
  return ((word >> 24) & 0xFE) == 0x4A;
}

void Cpu::RaiseException(uint32_t address, Exceptions exception, ExceptionCodes code) {
  // Counted so that a compiled memory access can tell, afterwards, that the
  // access it just made raised one - see exceptions_raised().
  ++exceptions_raised_;


  #if defined(_DEBUG) && defined(CPU_DEBUG)
    if(system_->csvlog.fp)
      fprintf_s(system_->csvlog.fp,"0x%08X,0x%08X,Exception,address,0x%08X,exception,0x%08X,code=0x%08X,SR,0x%08X\n",index,context_->prev_pc,address,exception,code,context_->ctrl.SR.raw);
  #endif
  //save to epc
  context_->ctrl.EPC = context_->branch_flag == true ? address-4 : address;

  if (code == kExceptionCodeDBE || code == kExceptionCodeIBE || 
      code == kExceptionCodeAdEL || code == kExceptionCodeAdES || 
      code == kExceptionCodeRI || code == kExceptionCodeBp || code == kExceptionCodeCpU) {
    static bool dumped = false;
    if (!dumped) {
      DumpTrace("trace_fatal_exception.txt", code);
      dumped = true;
    }
  }

  //push the bit stack for kernel,interrupt flags
  uint32_t& sr = context_->ctrl.SR.raw;
  const uint32_t status_before = sr;
  sr = (sr & ~0x3F) | ((sr & 0xF) << 2);


  // Cause, of which an exception writes only two fields: the code for what
  // happened (bits 2-6) and BD, whether it happened in a branch delay slot
  // (bit 31). Everything else is left exactly as it was - the interrupt-pending
  // field (bits 8-15) belongs to the interrupt lines and to software, not to
  // whatever exception happened to be taken.
  //
  // This used to clear the whole register and then, for an interrupt, fill the
  // pending field in from SR's *mask* - so Cause reported "every line you are
  // listening to is pending", which for the BIOS handler is indistinguishable
  // from the truth (it computes cause & sr & 0xFF00 and only needs it to be
  // non-zero) and wrong for anything that reads Cause to find out *which* line
  // it was. IOInterface::RefreshInterruptLine now maintains bit 10 from the
  // interrupt controller, MTC0 owns bits 8-9, and this leaves both alone.
  uint32_t& cause = context_->ctrl.Cause;
  cause = (cause & ~0xF000007Cu) |
          ((code & 0x1F) << 2) |
          (context_->branch_flag == true ? 0x80000000u : 0u);


  //specific exception handling
  if (exception == kTLBMissException) {
    if ((context_->ctrl.SR.BEV) == 0) //BEV = 0
      context_->pc = 0x80000000;
    else
      context_->pc = 0xBFC00100;
  }

  if (exception == kOtherException) {
    if ((context_->ctrl.SR.BEV) == 0) //BEV = 0
      context_->pc = 0x80000080;
    else
      context_->pc = 0xBFC00180;
  }

  ExceptionLog::Record(ExceptionLog::kException, address, context_->ctrl.EPC,
                       cause, status_before, sr);

  if (exception == kResetException) {
    //default state : 0101 0000 0110 0001 0000 0000 0000 0000
    context_->ctrl.SR.raw = 0x10900000;//0x50610000;
    context_->ctrl.PRId = 0x00000002;
    context_->pc = 0xBFC00000;
  }

}

void Cpu::Tick() {
  ++context_->cycles;
  ++context_->current_cycles;
  system_->io().Tick(1);
}

// A DMA holds the bus for the length of its transfer. The CPU does not
// execute during that time, but the GPU, the timers, the CD and the SPU all
// keep running - so those cycles have to reach them, and have to count
// towards the front end's idea of how much work a frame took.
void Cpu::TickCycles(uint32_t cycles) {
  context_->cycles += cycles;
  context_->current_cycles += cycles;
  // Handed on in the same size steps ordinary execution uses. One enormous
  // step would jump the GPU dozens of scanlines at once and leave the display
  // gates the counters watch meaningless for the whole transfer.
  while (cycles > 32) {
    system_->io().Tick(32);
    cycles -= 32;
  }
  if (cycles > 0)
    system_->io().Tick(cycles);
}

void Cpu::AccountCycles(uint32_t cycles) {
  context_->cycles += cycles;
  context_->current_cycles += cycles;
}
/*
uint32_t Cpu::LoadMemory(bool cached, int size_bytes, uint32_t physical_address, uint32_t virtual_address) {
  if (IsBusError() == true) {
    //context_->ctrl.BadVaddr = context_->prev_pc; //bus errors leave it
    auto code = current_stage == 1 ? kExceptionCodeIBE : kExceptionCodeDBE;
    RaiseException(context_->prev_pc,kOtherException,code);
    return 0;
  }

  if (IsAddressError(virtual_address,size_bytes) == true) {
    context_->ctrl.BadVaddr = virtual_address;
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }

  if ((context_->ctrl.SR.IsC) && current_stage != 1) { //cache isolation
    uint32_t data;
    //dont check if ((context_->ctrl.SR.SwC) == 0) { //check for swap!
      //dcache_.Read(physical_address,data);
      //if (size_bytes != 4)
//        dcache_.InvalidateLine(physical_address);
      switch (size_bytes) {
        case kM8: data = system_->io().scratchpad.u32[(physical_address&0x3FF)];
        case kM16: data = system_->io().scratchpad.u32[(physical_address&0x3FF)>>1];
        case kM32: data = system_->io().scratchpad.u32[(physical_address&0x3FF)>>2];
      }
    //} else {
    //  icache_.Read(physical_address,data);
    //  if (size_bytes != 4)
    //    icache_.InvalidateLine(physical_address);
    //}
    return data;
  }

  

  Buffer* buffer = nullptr;
  uint32_t target_address=0;
  if (physical_address >= 0x1FC00000 && physical_address <= 0x1FC80000) {
    buffer = &system_->io().bios_buffer;
    target_address = physical_address & 0x0007FFFF;
  }

  if (physical_address >= 0x00000000 && physical_address <= 0x001FFFFF) {
    buffer = &system_->io().ram_buffer;
    target_address = physical_address & 0x001FFFFF;
  }

  if (physical_address >= 0x1F000000 && physical_address <= 0x1F00FFFF) {
    buffer = &system_->io().parallel_port_buffer;
    target_address = physical_address & 0x0000FFFF;
  }

  if (physical_address >= 0x1F800000 && physical_address <= 0x1F8003FF) {
    buffer = &system_->io().scratchpad;//.u32[physical_address&0x3FF];
    target_address = physical_address & 0x3FF;
    //return dcache_.lines[physical_address&0x3FF].data[0];
  }

  if (physical_address >= 0x1F801000 && physical_address <= 0x1F802FFF) {
    switch (size_bytes) {
      case 1: return system_->io().Read08(physical_address);
      case 2: return system_->io().Read16(physical_address);
      case 4: return system_->io().Read32(physical_address);
    }
  }
  
  if (buffer != nullptr) {
    /*if (cached == true) {
      uint32_t data;
      auto cache_hit = icache_.Read(physical_address,data);

      if (cache_hit == true) {
        const uint32_t mask[] = {0x0,0xFF,0xFFFF,0xFFFFFF,0xFFFFFFFF};
        data = ( data >> ((physical_address&0x3)<<3)) & mask[size_bytes];
        return data;
      }
      else {
        icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
        //Tick();Tick();Tick();Tick();Tick();Tick();
      }

    }*/
/*
    switch (size_bytes) {
      case 1: return buffer->u8[target_address];
      case 2: return buffer->u16[target_address>>1];
      case 4: return buffer->u32[target_address>>2];
    }
  }

  BREAKPOINT
  return 0;
}

void Cpu::StoreMemory(bool cached, int size_bytes,uint32_t data, uint32_t physical_address, uint32_t virtual_address) {
  //todo: research about this value, ignore for now
  if (IsBusError() == true) { 
    //context_->ctrl.BadVaddr = context_->prev_pc; //bus errors leave it
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeDBE);
    return;
  }
  if ((IsAddressError(virtual_address,size_bytes) == true)) {
    // BadVaddr is the address that faulted, not the instruction that did it -
    // the pc is already in EPC. Storing the pc here made every address error
    // report BadVaddr == EPC, which reads like a jump into nowhere and hides
    // the pointer that was actually bad.
    context_->ctrl.BadVaddr = virtual_address;
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }

  if (context_->ctrl.SR.IsC) { //cache isolation
    uint32_t cdata[4] = { data };
    //if ((context_->ctrl.SR.SwC) == 0) { //check for swap!
      //dcache_.Write(physical_address,cdata);
      //if (size_bytes != 4)
        //dcache_.InvalidateLine(physical_address);
      assert(size_bytes==4);
      system_->io().scratchpad.u32[(physical_address&0x3FF)>>2] = data;
    /*} else {
      icache_.Write(physical_address,cdata);
      if (size_bytes != 4)
        icache_.InvalidateLine(physical_address);
    }*/
  /*  return;
  }

  Buffer* buffer = nullptr;
  uint32_t target_address=0;
  if (physical_address >= 0x1FC00000 && physical_address <= 0x1FC80000) {
    buffer = &system_->io().bios_buffer;
    target_address = physical_address & 0x0007FFFF;
  }

  if (physical_address >= 0x00000000 && physical_address <= 0x001FFFFF) {
    buffer = &system_->io().ram_buffer;
    target_address = physical_address & 0x001FFFFF;
  }

  if (physical_address >= 0x1F000000 && physical_address <= 0x1F00FFFF) {
    buffer = &system_->io().parallel_port_buffer;
    target_address = physical_address & 0x0000FFFF;
  }

   if (physical_address >= 0x1F800000 && physical_address <= 0x1F8003FF) {
    buffer = &system_->io().scratchpad;//[physical_address&0x3FF] = data;
    target_address = physical_address & 0x3FF;

    //dcache_.lines[physical_address&0x3FF].data[0] = data;
    //return;
  }

  if ((physical_address >= 0x1F801000 && physical_address <= 0x1F802FFF)||
    (physical_address >= 0xFFFE0000 && physical_address <= 0xFFFE0134)) {
    switch (size_bytes) {
      case 1: system_->io().Write08(physical_address,data&0xFF); return;
      case 2: system_->io().Write16(physical_address,data&0xFFFF); return;
      case 4: system_->io().Write32(physical_address,data); return;
    }
  }
  
  if (buffer != nullptr) {
    switch (size_bytes) {
      case 1: buffer->u8[target_address] = data; break;
      case 2: buffer->u16[target_address>>1] = data; break;
      case 4: buffer->u32[target_address>>2] = data; break;
    }
    /*if (cached == true) {
      //fprintf(system_->csvlog.fp,",,cache write,0x%08x,cache data,,actual data,0x%08X\n",physical_address,data);
      icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
    }*/
 /*   return;
  }

  BREAKPOINT
}
*/

uint32_t Cpu::Load(MemorySize size, uint32_t address) {
  if (IsBusError() == true) {
    // Only the first one is worth recording. This opened, appended to and
    // closed the file on every bus error, and a game that faults in a loop
    // faults millions of times - which turned a diagnostic into the slowest
    // thing in the run.
    static bool logged = false;
    if (current_stage != 1 && !logged) {
      logged = true;
      FILE* f = fopen("dbe_debug.txt", "w");
      if (f) {
        fprintf(f, "DBE! address=0x%08X valid=%d pc=0x%08X\n", address,
                valid_address_flag_, context_->pc);
        fclose(f);
      }
    }
    //context_->ctrl.BadVaddr = context_->prev_pc; //bus errors leave it
    auto code = current_stage == 1 ? kExceptionCodeIBE : kExceptionCodeDBE;
    RaiseException(context_->prev_pc,kOtherException,code);
    return 0;
  }

  if (IsAddressError(address,size) == true) {
    context_->ctrl.BadVaddr = address;
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }

  // With the cache isolated a data read comes from the cache and never from
  // memory. Nothing here holds cache data to hand back, so report a miss as
  // zero. It used to return a slice of the scratchpad instead, which is a
  // different block of memory entirely - isolation does not cover it - and the
  // switch had no breaks, so every size fell through to the 32-bit read. An
  // instruction fetch is exempt: the BIOS runs its cache sweep with the cache
  // isolated and has to keep fetching while it does.
  if ((context_->ctrl.SR.IsC) && current_stage != 1) {
    return 0;
  }

  // A read the debugger is watching for. Not a fetch, and not SWL/SWR's merge.
  if (debug_watch_) [[unlikely]]
    WatchLoad(size, address);


  // Decode on the physical address, not the virtual one. Every register has
  // three virtual addresses - KUSEG 0x1F80xxxx, KSEG0 0x9F80xxxx and KSEG1
  // 0xBF80xxxx - and the BIOS uses all three. Matching the virtual address
  // meant a register read through KSEG1 fell off the end of the decode and
  // returned zero without so much as a trap.
  const uint32_t physical = AddressTranslation(address);

  // Charge what the access actually costs. The R3000A issues a load in one
  // cycle but then stalls on the bus, and only main RAM and the scratchpad are
  // anywhere near fast. Without this the CPU runs roughly twice as many
  // instructions per frame as real hardware, and the BIOS notices: its VSync
  // gives up waiting for a vertical blank that has not had time to arrive and
  // prints "VSync: timeout", which is why the intro used to race past.
  //
  // Instruction fetches are excluded: those come through the instruction
  // cache, which is a separate cost and is not modelled here.
  //
  // The stall is everything a load costs beyond the one cycle every
  // instruction does, so a load's whole cost is 1 + stall, interpreted or
  // compiled. Checked against a real console by JaCzekanski's cpu/access-time,
  // which timing_test runs against this core (Test-Suite.md):
  //   - RAM 5, the scratchpad 1, the on-die registers 3 and the cache control
  //     register 1 are fixed costs, measured.
  //   - The BIOS ROM, the expansion regions, the CD-ROM and the SPU sit on 8-
  //     or 16-bit buses whose delays the BIOS programs into 1F801008h-
  //     1F801020h; their costs come from those registers and the width of the
  //     read (IOInterface::UpdateBusTiming). A word from the 8-bit ROM is four
  //     bus accesses: 25 cycles, where a byte is 7. With measured_bus_timing on,
  //     NarrowLoadStall uses the rule fitted to the console instead.
  //
  // SWL and SWR read the word they merge into, but that is this emulator's
  // way of doing a partial store, not a bus read: it costs nothing, and the
  // store costs what a store does.
  if (current_stage != 1 && !merging_store_) {
    const uint32_t width = (size == kM8) ? 0 : (size == kM16) ? 1 : 2;
    uint32_t stall = 0;
    // With the write queue modelled, a load that needs the bus waits for the
    // stores ahead of it. The scratchpad and cache control are on the chip.
    if (write_queue_) [[unlikely]] {
      const bool on_chip = address >= 0xFFFE0000 ||
                           (physical >= 0x1F800000 && physical <= 0x1F8003FF);
      if (!on_chip)
        DrainWriteQueue();
    }
    if (address >= 0xFFFE0000)                                    stall = 0;   // cache control
    else if (physical <= 0x007FFFFF)                              stall = 4;   // RAM
    else if (physical >= 0x1F800000 && physical <= 0x1F8003FF)    stall = 0;   // scratchpad
    else if (physical >= 0x1F801800 && physical <= 0x1F80180F)
      stall = NarrowLoadStall(IOInterface::kBusCdrom, width);
    else if (physical >= 0x1F801C00 && physical <= 0x1F801FFF)
      stall = NarrowLoadStall(IOInterface::kBusSpu, width);
    else if (physical >= 0x1F802000 && physical <= 0x1F802FFF)
      stall = NarrowLoadStall(IOInterface::kBusExp2, width);
    else if (physical >= 0x1F801000 && physical <= 0x1F801FFF)    stall = 2;   // on-die registers
    else if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF)
      stall = NarrowLoadStall(IOInterface::kBusBios, width);
    else if (physical >= 0x1F000000 && physical <= 0x1F7FFFFF)
      stall = NarrowLoadStall(IOInterface::kBusExp1, width);
    else if (physical >= 0x1FA00000 && physical <= 0x1FBFFFFF)
      stall = NarrowLoadStall(IOInterface::kBusExp3, width);
    else                                                          stall = 6;   // nothing there
    for (uint32_t i = 0; i < stall; ++i)
      Tick();
  }

  Buffer* buffer = nullptr;
  uint32_t offset = 0;

  if (physical <= 0x007FFFFF) {                 // RAM, and its mirrors
    // Read RAM directly. This used to go through ICache2, which is an
    // *instruction* cache: once the BIOS enabled it, every data load returned
    // whatever happened to be in a line buffer that is indexed by byte address
    // but filled one word at a time. A cache is a performance model, and
    // modelling it wrongly is worse than not modelling it at all.
    buffer = &system_->io().ram_buffer;
    offset = physical & 0x001FFFFF;
  } else if (physical >= 0x1F000000 && physical <= 0x1F00FFFF) {
    buffer = &system_->io().parallel_port_buffer;
    offset = physical & 0x0000FFFF;
  } else if (physical >= 0x1F800000 && physical <= 0x1F8003FF) {
    buffer = &system_->io().scratchpad;
    offset = physical & 0x000003FF;
  } else if (physical >= 0x1F801000 && physical <= 0x1F802FFF) {
    switch (size) {
      case kM8:  return system_->io().Read08(physical);
      case kM16: return system_->io().Read16(physical);
      case kM32: return system_->io().Read32(physical);
    }
  } else if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF) {
    buffer = &system_->io().bios_buffer;
    offset = physical & 0x0007FFFF;
  } else if (address >= 0xFFFE0000 && address <= 0xFFFE0FFF) {
    // Cache control sits in KSEG2, which is not translated at all.
    switch (size) {
      case kM8:  return system_->io().Read08(address);
      case kM16: return system_->io().Read16(address);
      case kM32: return system_->io().Read32(address);
    }
  }

  if (buffer != nullptr) {
   /* if (cached == true) {
      uint32_t data;
      auto cache_hit = icache_.Read(physical_address,data);

      if (cache_hit == true) {
        const uint32_t mask[] = {0x0,0xFF,0xFFFF,0xFFFFFF,0xFFFFFFFF};
        data = ( data >> ((physical_address&0x3)<<3)) & mask[size_bytes];
        return data;
      }
      else {
        icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
        //Tick();Tick();Tick();Tick();Tick();Tick();
      }

    }*/

    switch (size) {
      case kM8: return buffer->u8[offset];
      case kM16: return buffer->u16[offset>>1];
      case kM32: return buffer->u32[offset>>2];
    }
  }

  //BREAKPOINT
  return 0;
}

void Cpu::Store(MemorySize size, uint32_t data, uint32_t address) {
  //todo: research about this value, ignore for now
  if (IsBusError() == true) {
    //context_->ctrl.BadVaddr = context_->prev_pc; //bus errors leave it
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeDBE);
    return;
  }
  if ((IsAddressError(address,size) == true)) {
    context_->ctrl.BadVaddr = address;
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }

  // A store may be landing on instructions something has already compiled.
  // Nothing is registered unless the recompiler is switched on, and then this
  // is a predicted call and a bitmap lookup on the far side.
  if (store_observer_ != nullptr)
    store_observer_(store_observer_context_, address, static_cast<uint32_t>(size));

  // A watched RAM address records who wrote it. "This structure holds garbage"
  // is otherwise a dead end: the write that put it there happened long before
  // the read that noticed.
  if (watch_address_ != 0) {
    const uint32_t low = address & 0x1FFFFFFF;
    const uint32_t bytes = (size == kM8) ? 1u : (size == kM16) ? 2u : 4u;
    if (low + bytes > (watch_address_ & 0x1FFFFFFF) &&
        low < (watch_address_ & 0x1FFFFFFF) + 4) {
      Watch& w = watch_[watch_count_ % kWatchCapacity];
      w.pc = context_->prev_pc;
      w.address = address;
      w.value = data;
      w.size = bytes;
      ++watch_count_;
    }
  }


  // Isolating the cache points stores at the cache instead of memory, and that
  // is the whole mechanism the BIOS uses to drop instruction-cache lines: it
  // isolates, writes over 0x0000..0x0FFF one word per 16-byte line, and
  // un-isolates. So invalidate the line and write nothing.
  //
  // These stores used to land in the scratchpad at address & 0x3FF. The
  // scratchpad is separate fast RAM at 0x1F800000 that isolation has nothing
  // to do with, and the mask folded the BIOS's 4 KB sweep over it four times -
  // 15,000 stores of zero across the whole 1 KB. Harmless at boot, when there
  // is nothing in it yet, but a game that drops the instruction cache mid-play
  // would have had its scratchpad wiped underneath it. The switch had no
  // breaks either, so a byte store also did the halfword and word writes.
  if (context_->ctrl.SR.IsC) {
    icache.InvalidateLine(address);
    if (icache_tags_)
      icache_tags_[(address >> 4) & (kICacheLines - 1)] = 0xFFFFFFFFu;
    return;
  }

  // A write the debugger is watching for - after the isolated-cache case, which writes nothing.
  if (debug_watch_) [[unlikely]]
    WatchStore(size, address, data);

  // Same physical-address decode as Load; see the comment there.
  const uint32_t physical = AddressTranslation(address);

  // The write queue (EmuConfig::write_queue_timing). Everything but the scratchpad
  // and cache control goes out over the bus.
  if (write_queue_) [[unlikely]] {
    const bool on_chip = address >= 0xFFFE0000 ||
                         (physical >= 0x1F800000 && physical <= 0x1F8003FF);
    if (!on_chip)
      QueueStore(StoreOccupancy(physical, size));
  }

  Buffer* buffer = nullptr;
  uint32_t offset = 0;

  if (physical <= 0x007FFFFF) {                 // RAM, and its mirrors
    icache.InvalidateLine(address);
    buffer = &system_->io().ram_buffer;
    offset = physical & 0x001FFFFF;
  } else if (physical >= 0x1F000000 && physical <= 0x1F00FFFF) {
    buffer = &system_->io().parallel_port_buffer;
    offset = physical & 0x0000FFFF;
  } else if (physical >= 0x1F800000 && physical <= 0x1F8003FF) {
    buffer = &system_->io().scratchpad;
    offset = physical & 0x000003FF;
  } else if (physical >= 0x1F801000 && physical <= 0x1F802FFF) {
    switch (size) {
      case kM8:  system_->io().Write08(physical, data & 0xFF); return;
      case kM16: system_->io().Write16(physical, data & 0xFFFF); return;
      case kM32: system_->io().Write32(physical, data); return;
    }
  } else if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF) {
    // The BIOS is read-only; a write there is discarded, not an error.
    return;
  } else if (address >= 0xFFFE0000 && address <= 0xFFFE0FFF) {
    switch (size) {
      case kM8:  system_->io().Write08(address, data & 0xFF); return;
      case kM16: system_->io().Write16(address, data & 0xFFFF); return;
      case kM32: system_->io().Write32(address, data); return;
    }
  }

  if (buffer != nullptr) {
    switch (size) {
      case kM8: buffer->u8[offset] = data; break;
      case kM16: buffer->u16[offset>>1] = data; break;
      case kM32: buffer->u32[offset>>2] = data; break;
    }
    /*if (cached == true) {
      //fprintf(system_->csvlog.fp,",,cache write,0x%08x,cache data,,actual data,0x%08X\n",physical_address,data);
      icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
    }*/
    return;
  }

  BREAKPOINT
}


void Cpu::StageIF() {
  current_stage = 1;
  auto ppc = AddressTranslation(context_->pc);
  // The instruction-cache model (bug 94). A flag of the CPU's own, brought in line
  // with the setting once a batch (SyncICacheSetting) rather than read through
  // System here on every instruction: measured in pairs against the tree before it,
  // reading the setting here cost the interpreter 1%, and the latch that preceded
  // that - three loads a step in System - cost 3.6% along with the tags' old place
  // in the class.
  if (icache_timing_) [[unlikely]] {
    const uint32_t stall = FetchStall(context_->pc);
    for (uint32_t i = 0; i < stall; ++i)
      Tick();
  }
  context_->code = Load(kM32,context_->pc); //LoadMemory(cache_flag_,4,ppc,context_->pc);
  context_->pc  += 4;
}

void Cpu::StageRD() {
  current_stage = 2;
  opcode_ = context_->opcode();
  immediate_ = context_->immediate();
  immediate_32bit_sign_extended_ = context_->immediate_32bit_sign_extended();
  target_ = context_->target();
  funct_ = context_->fu();
  shamt_ = context_->sa();
  rd_ = context_->rd();
  rt_ = context_->rt();
  rs_ = context_->rs();
}

void Cpu::Jump(uint32_t address) {
  // The branch's own cycle; the delay slot below charges its own. Every taken
  // branch, jump and jr/jalr comes through here, so the pair costs 2 - what
  // timers.exe's branch loops read on a real console, and what psxtest_gte's
  // TIMING checks expect of its loops (bug 78).
  Tick();
  // A target that is not word aligned faults as the branch is taken, before
  // its delay slot: AdEL, with EPC and BadVaddr both the target (amidog's
  // psxtest_cpu jalr group; DuckStation's CPU::Branch). Only jr and jalr can
  // produce one - every other branch builds its target in whole words.
  if ((address & 3) != 0) {
    context_->ctrl.BadVaddr = address;
    RaiseException(address, kOtherException, kExceptionCodeAdEL);
    return;
  }
  __inside_delay_slot = true;
  context_->branch_flag = true;
  // Whether the delay slot raised anything, asked of the exception counter
  // rather than by watching Cause for a change. Cause now carries the live
  // interrupt-pending bit, which a device can flip during the delay slot
  // without any exception being taken - and reading that as "an exception
  // happened" would leave the pc wherever the slot left it instead of at the
  // branch target.
  const uint64_t exceptions_before = exceptions_raised_;
  ExecuteInstruction();
  context_->branch_flag = false;
  __inside_delay_slot = false;
  // If an exception (like an interrupt) happened during the delay slot,
  // ExecuteInstruction would have called RaiseException and set PC to 0x80000080.
  // We should NOT overwrite PC with the jump target in this case.
  if (exceptions_raised_ == exceptions_before) {
    context_->pc = address;
  }
  if (output_inst == true && until_address == context_->pc)
    output_inst = false;
}

void Cpu::UNKNOWN() {
  #if defined(_DEBUG) && defined(CPU_DEBUG)
    if(system_->csvlog.fp)
      fprintf(system_->csvlog.fp,"unknown intstruction @ 0x%08X code=0x%08X\n",context_->prev_pc,context_->code);
  #endif
}

void Cpu::SPECIAL() {
  (this->*(machine_instruction_special_[funct_]))();
}

// Every one of the 32 rt encodings is a branch, not just the four documented
// ones: bit 0 picks BGEZ over BLTZ, and the link happens when rt is 10h or
// 11h - (rt & 1Eh) == 10h - so rt 12h-1Fh branch without linking. The
// comparison reads rs before the link writes r31, so bltzal $ra tests the
// old $ra, and the link is written whether or not the branch is taken
// (amidog's psxtest_cpu BRA ADV groups b_0x00..b_0x1f; DuckStation's decode).
// The table used to hold UNKNOWN for the other 28, which did nothing at all.
void Cpu::REGIMM() {
  const int32_t value = static_cast<int32_t>(context_->gp.reg[rs_]);
  const bool greater_or_equal = (rt_ & 1) != 0;
  const bool taken = (value < 0) != greater_or_equal;
  if ((rt_ & 0x1E) == 0x10)
    WriteReg(31, context_->pc + 4);
  if (taken) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  } else {
    Tick();
  }
}

void Cpu::J() {
  Jump((context_->pc & 0xF0000000) | (target_ << 2));
}

void Cpu::JAL() {
  WriteReg(31, context_->pc + 4);
  Jump((context_->pc & 0xF0000000) | (target_ << 2));
}

// A branch costs one cycle whether or not it is taken. Taken, Jump() charges
// it; not taken, the instruction after it runs as an ordinary next fetch
// rather than through Jump(), so the branch has to charge its own here.
// (Taken branches were once charged nothing beyond the delay slot, on the
// strength of an SQR-loop figure that had itself been derived from this
// core's own costs - bugs 43 and 78.)
void Cpu::BEQ() {
  if (context_->gp.reg[rs_] == context_->gp.reg[rt_]) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  } else {
    Tick();
  }
}

void Cpu::BNE() {
  if (context_->gp.reg[rs_] != context_->gp.reg[rt_]) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  } else {
    Tick();
  }
}

void Cpu::BLEZ() {
  const int32_t r = static_cast<int32_t>(context_->gp.reg[rs_]);
  if (r <= 0) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  } else {
    Tick();
  }
}

void Cpu::BGTZ() {
  const int32_t r = static_cast<int32_t>(context_->gp.reg[rs_]);
  if (r > 0) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  } else {
    Tick();
  }
}

// Signed overflow traps, and the destination is left as it was - the same as
// ADD, which always did this; ADDI and SUB did not.
void Cpu::ADDI() {
  const uint32_t a = context_->gp.reg[rs_];
  const uint32_t b = static_cast<uint32_t>(immediate_32bit_sign_extended_);
  const uint32_t result = a + b;
  if ((~(a ^ b) & (a ^ result)) & 0x80000000u) {
    RaiseException(context_->prev_pc, kOtherException, kExceptionCodeOv);
  } else {
    WriteReg(rt_, result);
  }
  Tick();
}

void Cpu::ADDIU() {
  WriteReg(rt_, context_->gp.reg[rs_] + immediate_32bit_sign_extended_);
  Tick();
}

void Cpu::SLTI() {
  WriteReg(rt_, (int32_t)context_->gp.reg[rs_] < immediate_32bit_sign_extended_);
  Tick();
}

// Unsigned, but the immediate is still sign-extended first: sltiu rt, rs, -1
// compares against FFFFFFFFh, not FFFFh.
void Cpu::SLTIU() {
  WriteReg(rt_, context_->gp.reg[rs_] <
                    static_cast<uint32_t>(immediate_32bit_sign_extended_));
  Tick();
}


void Cpu::ANDI() {
  WriteReg(rt_, context_->gp.reg[rs_] & immediate_);
  Tick();
}

void Cpu::ORI() {
  WriteReg(rt_, context_->gp.reg[rs_] | immediate_);
  Tick();
}

void Cpu::XORI() {
  WriteReg(rt_, context_->gp.reg[rs_] ^ immediate_);
  Tick();
}

void Cpu::LUI() {
  WriteReg(rt_, immediate_ << 16);
  Tick();
  //WriteReg(rt_, context_->immediate_ << 16);
}

// See the declaration in cpu.h: the pending field's hardware half is live,
// and only bit 10 of it is wired to anything on this machine.
uint32_t Cpu::CauseRegister() const {
  const uint32_t stored = context_->ctrl.Cause & ~0x0000FC00u;
  return stored | (system_->io().interrupt_line() ? 0x00000400u : 0u);
}

void Cpu::COP0() {
  switch (context_->rs()) {
    //MFC
    case 0x00: {
      WriteReg(rt_, (rd_ == 13) ? CauseRegister() : context_->ctrl.reg[rd_]);
      break;
    }
    //MTC
    case 0x04: {
      const uint32_t before = context_->ctrl.SR.raw;
      if (rd_ == 13) {
        // Cause is read-only apart from the two software-interrupt bits: the
        // rest is the hardware's account of what happened, and writing it
        // would let software invent an exception code or claim a device
        // interrupt. Bits 8-9 are software's own pending lines, and taken as
        // seriously as bit 10 - see System::StepImpl.
        context_->ctrl.Cause =
            (context_->ctrl.Cause & ~0x00000300u) |
            (context_->gp.reg[rt_] & 0x00000300u);
      } else {
        context_->ctrl.reg[rd_] = context_->gp.reg[rt_];
      }
      if (rd_ == 12) {
        ExceptionLog::Record(ExceptionLog::kStatusWrite, context_->prev_pc,
                             context_->ctrl.EPC, context_->ctrl.Cause, before,
                             context_->ctrl.SR.raw);
      }
      break;
    }
    //RFE
    case 0x10: {
      ++TrapCounter::rfe_count;
      const uint32_t before = context_->ctrl.SR.raw;
      context_->ctrl.SR.raw = (before & ~0xF) | ((before >> 2) & 0xF);
      ExceptionLog::Record(ExceptionLog::kReturn, context_->prev_pc,
                           context_->ctrl.EPC, context_->ctrl.Cause, before,
                           context_->ctrl.SR.raw);
      break;
    }
    default:
      BREAKPOINT_DETAIL(context_->code);
  }
  Tick();
}

// Coprocessor 2 is the GTE. Bit 25 of the instruction picks between a command
// and a register move; the move form is selected by rs, exactly as for Cop0.
//
// Only the command form was handled before, and it trapped afterwards anyway,
// so every MFC2/MTC2/CFC2/CTC2 - which is how software gets its vertices in
// and its results out - did nothing at all.
void Cpu::COP2() {
  const bool is_command = (context_->code & (1u << 25)) != 0;
  const uint32_t rs = context_->rs();
  // "If an instruction that reads a GTE register or a GTE command is
  // executed before the current GTE command is finished, the CPU will hold
  // until [it] has finished" - psx-spx. MFC2/CFC2 are the register reads;
  // MTC2/CTC2 load new operands and are not documented to wait on this.
  const bool reads_register = !is_command && (rs == 0x00 || rs == 0x02);
  //
  // A hold costs one cycle more than the wait: the CPU restarts the cycle
  // after the command finishes. An access that arrives exactly as it
  // finishes holds for nothing. Both are how psxtest_gte's TIMING checks
  // score its loops - an SQR (5) with 0 or 1 nops before CFC2 costs the same
  // 11 cycles a pass, with 4 nops it costs 10 (bug 78) - and DuckStation's
  // AddGTETicks carries the same extra cycle.

  if ((is_command || reads_register) &&
      context_->cycles < gte_busy_until_cycles_) {
    TickCycles(
        static_cast<uint32_t>(gte_busy_until_cycles_ - context_->cycles) + 1);
  }

  if (is_command) {
    // One cycle to issue, like any instruction; Execute()'s return is the
    // command's whole documented duration (5 to 44 cycles - see gte.cpp),
    // the remainder of which the GTE spends busy in the background and the
    // next hazard above charges to whatever touches it too soon.
    const uint32_t total_cycles = system_->gte().Execute(context_->code);
    Tick();
    gte_busy_until_cycles_ =
        context_->cycles + (total_cycles > 0 ? total_cycles - 1 : 0);
    return;
  }

  switch (rs) {
    case 0x00:  // MFC2
      // Like an ordinary load: the value is promised now and lands one
      // instruction later, not immediately - psx-spx notes Tekken 2's
      // geometry depends on getting this right.
      ArmLoad(rt_, system_->gte().ReadData(rd_));
      break;
    case 0x02:  // CFC2
      ArmLoad(rt_, system_->gte().ReadControl(rd_));
      break;
    case 0x04:  // MTC2
      system_->gte().WriteData(rd_, context_->gp.reg[rt_]);
      break;
    case 0x06:  // CTC2
      system_->gte().WriteControl(rd_, context_->gp.reg[rt_]);
      break;
    default:
      BREAKPOINT_DETAIL(context_->code);
      break;
  }
  Tick();
}

// LWC2 and SWC2 move a GTE data register straight to or from memory. They were
// UNKNOWN in the opcode table, so a display list built with them silently
// transferred nothing.
void Cpu::LWC2() {
  const uint32_t address =
      context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  system_->gte().WriteData(rt_, Load(kM32, address));
  Tick();
}

void Cpu::SWC2() {
  const uint32_t address =
      context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  Store(kM32, system_->gte().ReadData(rt_), address);
  Tick();
}

void Cpu::LB() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  const uint64_t faults = exceptions_raised_;
  uint8_t mem = Load(kM8,virtual_address);
  Tick();
  if (LoadFaulted(faults)) return;
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>((int8_t)mem));
}

void Cpu::LH() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  const uint64_t faults = exceptions_raised_;
  uint16_t mem = Load(kM16,virtual_address);
  Tick();
  if (LoadFaulted(faults)) return;
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>((int16_t)mem));
}

void Cpu::LWL() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  // From the bottom of the word to the addressed byte is all it reads - which only
  // the measured bus rule charges by (NarrowLoadStall).
  partial_lane_ = 0;
  partial_bytes_ = static_cast<uint8_t>((virtual_address & 3) + 1);
  uint32_t mem = Load(kM32,virtual_address & ~0x03);
  partial_bytes_ = 0;
  Tick();
  switch (virtual_address & 0x3) {
    case 0:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0x00FFFFFF) | (mem<<24));
      break;
    case 1:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0x0000FFFF) | (mem<<16));
      break;
    case 2:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0x000000FF) | (mem<<8));
      break;
    case 3:
      ArmLoad(rt_, mem);
      break;
  }
}

void Cpu::LW() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  const uint64_t faults = exceptions_raised_;
  uint32_t mem;
  mem = Load(kM32,virtual_address);
  Tick();
  if (LoadFaulted(faults)) return;
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>(mem));
}

void Cpu::LBU() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  const uint64_t faults = exceptions_raised_;
  uint32_t mem = Load(kM8,virtual_address);
  Tick();
  if (LoadFaulted(faults)) return;
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>((uint8_t)mem));
}

void Cpu::LHU() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  const uint64_t faults = exceptions_raised_;
  uint32_t mem = Load(kM16,virtual_address);
  Tick();
  if (LoadFaulted(faults)) return;
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>((uint16_t)mem));
}

void Cpu::LWR() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  // From the addressed byte to the top of the word is all it reads - which only
  // the measured bus rule charges by (NarrowLoadStall).
  partial_lane_ = static_cast<uint8_t>(virtual_address & 3);
  partial_bytes_ = static_cast<uint8_t>(4 - (virtual_address & 3));
  uint32_t mem = Load(kM32,virtual_address & ~0x03);
  partial_bytes_ = 0;
  Tick();
  switch (virtual_address & 0x3) {
    case 0:
      ArmLoad(rt_, mem);
      break;
    case 1:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0xFF000000) | (mem>>8));
      break;
    case 2:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0xFFFF0000) | (mem>>16));
      break;
    case 3:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0xFFFFFF00) | (mem>>24));
      break;
  }
}

void Cpu::SB() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  //StoreMemory(cache_flag_,1,context_->gp.reg[rt_],physical_address,virtual_address);
  Store(kM8,context_->gp.reg[rt_],virtual_address);
  Tick();
}

void Cpu::SH() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  //StoreMemory(cache_flag_,2,context_->gp.reg[rt_],physical_address,virtual_address);
  Store(kM16,context_->gp.reg[rt_],virtual_address);
  Tick();
}

void Cpu::SWL() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  // The bytes this store does not cover have to survive, so the existing word
  // has to be read before the register is merged into it. Leaving `data`
  // uninitialised here corrupted three bytes out of every four, which broke
  // every unaligned copy the BIOS makes.
  merging_store_ = true;
  uint32_t data = Load(kM32, virtual_address & ~0x03);
  merging_store_ = false;
  switch (virtual_address & 0x3) {
    case 0:
      data = (data & 0xFFFFFF00) | (context_->gp.reg[rt_] >> 24);
      break;
    case 1:
      data = (data & 0xFFFF0000) | (context_->gp.reg[rt_] >> 16);
      break;
    case 2:
      data = (data & 0xFF000000) | (context_->gp.reg[rt_] >> 8);
      break;
    case 3:
      data = context_->gp.reg[rt_];
      break;
  }
  Store(kM32,data,virtual_address & ~0x03);

  Tick();
}

void Cpu::SW() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  Store(kM32,context_->gp.reg[rt_],virtual_address);
  Tick();
}

void Cpu::SWR() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  // Same as SWL: the uncovered bytes have to be preserved, so read first.
  merging_store_ = true;
  uint32_t data = Load(kM32, virtual_address & ~0x03);
  merging_store_ = false;
  switch (virtual_address & 0x3) {
    case 0:
      data = context_->gp.reg[rt_];
      break;
    case 1:
      data = (data & 0x000000FF) | (context_->gp.reg[rt_] << 8);
      break;
    case 2:
      data = (data & 0x0000FFFF) | (context_->gp.reg[rt_] << 16);
      break;
    case 3:
      data = (data & 0x00FFFFFF) | (context_->gp.reg[rt_] << 24);
      break;
  }
  Store(kM32,data,virtual_address & ~0x03);
  Tick();
}

void Cpu::SLL() {
  WriteReg(rd_, context_->gp.reg[rt_] << shamt_);
  Tick();
}

void Cpu::SRL() {
  WriteReg(rd_, context_->gp.reg[rt_] >> shamt_);
  Tick();
}

void Cpu::SRA() {
  WriteReg(rd_, (int32_t)context_->gp.reg[rt_] >> shamt_);
  Tick();
}

void Cpu::SLLV() {
  WriteReg(rd_, context_->gp.reg[rt_] << (context_->gp.reg[rs_] & 0x1F));
  Tick();
}

void Cpu::SRLV() {
 WriteReg(rd_, context_->gp.reg[rt_] >> (context_->gp.reg[rs_] & 0x1F));
 Tick();
}

void Cpu::SRAV() {
  WriteReg(rd_, (int32_t)context_->gp.reg[rt_] >> (context_->gp.reg[rs_] & 0x1F));
  Tick();
}

void Cpu::JR() {
  Jump(context_->gp.reg[rs_]);
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;
}

void Cpu::JALR() {
  // The target is read before the link is written: jalr rd, rs with rd == rs
  // jumps to the old rs. Any rd links, not only r31.
  const uint32_t target = context_->gp.reg[rs_];
  WriteReg(rd_, context_->pc + 4);
  Jump(target);
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;

}

void Cpu::SYSCALL() {
  RaiseException(context_->prev_pc,kOtherException,kExceptionCodeSyscall);
}

// `break` raises an exception like any other; it is how a debugger and the
// BIOS's own assertions stop the machine. It used to expand to nothing but a
// host-side debug marker, so the instruction simply fell through.
void Cpu::BREAK() {
  RaiseException(context_->prev_pc, kOtherException, kExceptionCodeBp);
}

// MFHI/MFLO read whatever a multiply or divide left behind. If that
// operation is still busy - see hilo_busy_until_cycles_ - the read has to
// wait for it, the same hazard COP2() already charges for a GTE register
// read that outruns the command that fills it.
void Cpu::MFHI() {
  if (context_->cycles < hilo_busy_until_cycles_)
    TickCycles(static_cast<uint32_t>(hilo_busy_until_cycles_ - context_->cycles));
  WriteReg(rd_, context_->high);
  Tick();
}

void Cpu::MTHI() {
  context_->high  = context_->gp.reg[rs_];
  Tick();
}

void Cpu::MFLO() {
  if (context_->cycles < hilo_busy_until_cycles_)
    TickCycles(static_cast<uint32_t>(hilo_busy_until_cycles_ - context_->cycles));
  WriteReg(rd_, context_->low);
  Tick();
}

void Cpu::MTLO() {
  context_->low  = context_->gp.reg[rs_];
  Tick();
}

// Multiply's execution time depends on the magnitude of rs - "small*large"
// can be much faster than "large*small" - per psx-spx's measured bands.
// MULT's ranges cover rs as a signed quantity split across the wrap; MULTU's
// are the same three widths read unsigned, with no negative-side band.
namespace {
uint32_t MultiplyCyclesSigned(int32_t rs) {
  const uint32_t bits = static_cast<uint32_t>(rs);
  if (bits <= 0x000007FFu || bits >= 0xFFFFF800u) return 6;
  if (bits <= 0x000FFFFFu || bits >= 0xFFF00000u) return 9;
  return 13;
}
uint32_t MultiplyCyclesUnsigned(uint32_t rs) {
  if (rs <= 0x000007FFu) return 6;
  if (rs <= 0x000FFFFFu) return 9;
  return 13;
}
}  // namespace

void Cpu::MULT() {
  uint64_t test = int64_t((int64_t)((int32_t)context_->gp.reg[rs_]) * (int64_t)((int32_t)context_->gp.reg[rt_]));
  context_->low  = (uint32_t)(test & 0xFFFFFFFF);
  context_->high = (uint32_t)((test >> 32) & 0xFFFFFFFF);
  const uint32_t cost = MultiplyCyclesSigned(static_cast<int32_t>(context_->gp.reg[rs_]));
  TickCycles(cost);
  hilo_busy_until_cycles_ = context_->cycles + (cost > 1 ? cost - 1 : 0);
}

void Cpu::MULTU() {
  uint64_t test = uint64_t((uint64_t)((uint32_t)context_->gp.reg[rs_]) * (uint64_t)((uint32_t)context_->gp.reg[rt_]));
  context_->low  = (uint32_t)(test & 0xFFFFFFFF);
  context_->high = (uint32_t)((test >> 32) & 0xFFFFFFFF);
  const uint32_t cost = MultiplyCyclesUnsigned(context_->gp.reg[rs_]);
  TickCycles(cost);
  hilo_busy_until_cycles_ = context_->cycles + (cost > 1 ? cost - 1 : 0);
}

// Division on MIPS never traps. Both degenerate cases have defined answers,
// and both have to be handled here rather than handed to the host CPU: x86
// raises a hardware divide-error for each of them, which takes the whole
// emulator down rather than producing a wrong number.
void Cpu::DIV() {
  const int32_t dividend = static_cast<int32_t>(context_->gp.reg[rs_]);
  const int32_t divisor = static_cast<int32_t>(context_->gp.reg[rt_]);

  if (divisor == 0) {
    // Quotient is all ones or one, depending on the sign of the dividend;
    // the remainder is the dividend itself.
    context_->high = static_cast<uint32_t>(dividend);
    context_->low = (dividend >= 0) ? 0xFFFFFFFFu : 1u;
  } else if (static_cast<uint32_t>(dividend) == 0x80000000u && divisor == -1) {
    // The one quotient that does not fit in 32 bits. The result is the
    // dividend unchanged, with no remainder.
    context_->high = 0;
    context_->low = 0x80000000u;
  } else {
    context_->low = static_cast<uint32_t>(dividend / divisor);
    context_->high = static_cast<uint32_t>(dividend % divisor);
  }
  // Fixed at 36 cycles regardless of operands - psx-spx.
  TickCycles(36);
  hilo_busy_until_cycles_ = context_->cycles + 35;
}

void Cpu::DIVU() {
  const uint32_t dividend = context_->gp.reg[rs_];
  const uint32_t divisor = context_->gp.reg[rt_];

  if (divisor == 0) {
    context_->high = dividend;
    context_->low = 0xFFFFFFFFu;
  } else {
    context_->low = dividend / divisor;
    context_->high = dividend % divisor;
  }
  TickCycles(36);
  hilo_busy_until_cycles_ = context_->cycles + 35;
}

void Cpu::ADD() {

  uint64_t a = context_->gp.reg[rs_];
  uint64_t b = context_->gp.reg[rt_];
  uint64_t temp = ((BIT(a,31)<<32) | a) + ((BIT(b,31)<<32) | b);
  if (BIT(temp,32) != BIT(temp,31)) {
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeOv);
  } else {
    WriteReg(rd_, temp & 0xffffffff);
  }
  //WriteReg(rd_, context_->gp.reg[rs_] + context_->gp.reg[rt_]);
  Tick();
}

void Cpu::ADDU() {
  WriteReg(rd_, context_->gp.reg[rs_] + context_->gp.reg[rt_]);
  Tick();
}

void Cpu::SUB() {
  const uint32_t a = context_->gp.reg[rs_];
  const uint32_t b = context_->gp.reg[rt_];
  const uint32_t result = a - b;
  if (((a ^ b) & (a ^ result)) & 0x80000000u) {
    RaiseException(context_->prev_pc, kOtherException, kExceptionCodeOv);
  } else {
    WriteReg(rd_, result);
  }
  Tick();
}

void Cpu::SUBU() {
  WriteReg(rd_, context_->gp.reg[rs_] - context_->gp.reg[rt_]);
  Tick();
}

void Cpu::AND() {
  WriteReg(rd_, context_->gp.reg[rs_] & context_->gp.reg[rt_]);
  Tick();
}

void Cpu::OR() {
  WriteReg(rd_, context_->gp.reg[rs_] | context_->gp.reg[rt_]);
  Tick();
}

void Cpu::XOR() {
  WriteReg(rd_, context_->gp.reg[rs_] ^ context_->gp.reg[rt_]);
  Tick();
}

void Cpu::NOR() {
  WriteReg(rd_, ~(context_->gp.reg[rs_] | context_->gp.reg[rt_]));
  Tick();
}

void Cpu::SLT() {
  WriteReg(rd_, (int32_t)context_->gp.reg[rs_] < (int32_t)context_->gp.reg[rt_]);
  Tick();
}

void Cpu::SLTU() {
  WriteReg(rd_, context_->gp.reg[rs_] < context_->gp.reg[rt_]);
  Tick();
}

void Cpu::BLTZ() {
  int32_t r = (int32_t)context_->gp.reg[rs_] ;
  bool cond = r < 0; //(context_->gp.reg[rs_] & 0x80000000)==0x80000000;
  if (cond==true) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  } else {
    Tick();
  }
}

void Cpu::BGEZ() {
  //int32_t r = (int32_t)context_->gp.reg[rs_] ;
  bool cond = (context_->gp.reg[rs_] & 0x80000000)==0;//r >= 0;//
  if (cond==true) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  } else {
    Tick();
  }
}

void Cpu::BLTZAL() {
  WriteReg(31, context_->pc + 4);
  BLTZ();
}

void Cpu::BGEZAL() {
  WriteReg(31, context_->pc + 4);
  BGEZ();
}

}
}
