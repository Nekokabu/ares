#include <n64/n64.hpp>
#include <nall/gdb/server.hpp>

namespace ares::Nintendo64 {

CPU cpu;
#include "context.cpp"
#include "dcache.cpp"
#include "tlb.cpp"
#include "memory.cpp"
#include "exceptions.cpp"
#include "algorithms.cpp"
#include "interpreter.cpp"
#include "decoder.cpp"
#include "interpreter-ipu.cpp"
#include "interpreter-scc.cpp"
#include "interpreter-fpu.cpp"
#include "interpreter-cop2.cpp"
#include "recompiler.cpp"
#include "recompiler-fpu.cpp"
#include "recompiler-ipu.cpp"
#include "debugger.cpp"
#include "serialization.cpp"
#include "disassembler.cpp"
#include "emux.cpp"

auto CPU::load(Node::Object parent) -> void {
  node = parent->append<Node::Object>("CPU");
  debugger.load(node);
}

auto CPU::unload() -> void {
  debugger.unload();
  node.reset();
}

auto CPU::main() -> void {
  while(!vi.refreshed && GDB::server.reportPC(ipu.pc & 0xFFFFFFFF)) {
    if(instruction()) synchronize();
  }

  vi.refreshed = false;
  queue.remove(Queue::GDB_Poll);
  if(GDB::server.hasClient()) {
    queueInsert(Queue::GDB_Poll, (93750000*2)/60/240);
  }
}

auto CPU::gdbPoll() -> void {
  if(GDB::server.hasClient()) {
    GDB::server.updateLoop();
    queueInsert(Queue::GDB_Poll, (93750000*2)/60/240);
  }
}

auto CPU::queueInsert(u32 event, u32 clocks) -> void {
  if(!queue.insert(event, clocks)) return;
  s64 queueDelta = queue.timeToNextEvent();
  if(queueDelta < 0) queueDelta = 0;
  s64 queueTarget = Thread::clock + queueDelta;
  if(queueTarget < jitClockTarget) jitClockTarget = queueTarget;
}

auto CPU::forceSynchronize() -> void {
  jitClockTarget = 0;
}

auto CPU::stepCount(u64 clocks) -> void {
  if(!clocks) return;
  u64 remaining = (u64)(scc.compare - scc.count) & CountMask;
  if(remaining && clocks >= remaining) setInterruptPending(Interrupt::Timer, 1);
  scc.count += clocks;
  profile.cpuCycles += clocks;
  if(scc.status.exceptionLevel) profile.cpuCyclesExc += clocks;
}

auto CPU::flushCount() -> void {
  auto clocks = pendingCount();
  countClock += clocks << 1;
  stepCount(clocks);
}

auto CPU::synchronize() -> void {
  auto clocks = Thread::clock;
  auto counted = countClock;
  clockEpoch += clocks;
  Thread::clock = 0;
  countClock = 0;
  jitClockTarget = 0;

   vi.clock -= clocks;
   ai.clock -= clocks;
  rsp.clock -= clocks;
  rdp.clock -= clocks;
  pif.clock -= clocks;
  vi.main();
  ai.main();
  rsp.main();
  rdp.main();
  pif.main();

  queue.step(clocks, [](u32 event) {
    switch(event) {
    case Queue::PI_DMA_Read:   return pi.dmaFinished();
    case Queue::PI_DMA_Write:  return pi.dmaFinished();
    case Queue::PI_BUS_Write:  return pi.writeFinished();
    case Queue::SI_DMA_Read:   return si.dmaRead();
    case Queue::SI_DMA_Write:  return si.dmaWrite();
    case Queue::SI_BUS_Write:  return si.writeFinished();
    case Queue::RTC_Tick:      return cartridge.rtc.tick();
    case Queue::EEPROM_Write:  return cartridge.eepromFinish();
    case Queue::Flash_Complete: return cartridge.flash.finish();
    case Queue::DD_Clock_Tick:  return dd.rtc.tickClock();
    case Queue::DD_MECHA_Response:  return dd.mechaResponse();
    case Queue::DD_BM_Request:  return dd.bmRequest();
    case Queue::DD_Motor_Mode:  return dd.motorChange();
    case Queue::GDB_Poll:      return cpu.gdbPoll();
    }
  });

  stepCount((clocks - counted) >> 1);
}

auto CPU::setInterruptPending(u32 bit, bool value) -> void {
  scc.cause.interruptPending.bit(bit) = value;
  interruptPoll();
}

auto CPU::interruptPoll() -> void {
  if(auto interrupts = scc.cause.interruptPending & scc.status.interruptMask) {
    if(scc.status.interruptEnable && !scc.status.exceptionLevel && !scc.status.errorLevel) {
      forceSynchronize();
    }
  }
}

auto CPU::instruction() -> bool {
  if(auto interrupts = scc.cause.interruptPending & scc.status.interruptMask) {
    if(scc.status.interruptEnable && !scc.status.exceptionLevel && !scc.status.errorLevel) {
      debugger.interrupt(scc.cause.interruptPending);
      step(1 * 2);
      exception.interrupt();
      return true;
    }
  }

  if (scc.nmiPending) {
    debugger.nmi();
    step(1 * 2);
    exception.nmi();
    return true;
  }
  if (scc.sysadFrozen) {
    step(1 * 2);
    return true;
  }

  auto access = devirtualize<Read, Word>(ipu.pc);
  if(!access) return true;

  if(Accuracy::CPU::Recompiler && recompiler.enabled && access.cache) {
    if(vaddrAlignedError<Word>(access.vaddr, false)) return true;
    auto block = recompiler.block(ipu.pc, access.paddr);
    if(block) {
      if(Thread::clock >= jitClockTarget) {
        s64 timerDelta = (s64)scc.compare - (s64)effectiveCount();
        if(timerDelta < 0) timerDelta = 0;
        s64 queueDelta = queue.timeToNextEvent();
        if(queueDelta < 0) queueDelta = 0;
        s64 capBudget = min<s64>(Accuracy::CPU::JitInterleaving, min(timerDelta, queueDelta));
        jitClockTarget = Thread::clock + capBudget;
      }
      block->execute(*this);
      return Thread::clock >= jitClockTarget;
    }
  }

  auto data = fetch(access);
  if (!data) return true;
  pipeline.begin();
  instructionPrologue(ipu.pc, *data);
  decoderEXECUTE(*data);
  instructionEpilogue<0>();
  pipeline.end();
  return true;
}

auto CPU::instructionPrologue(u64 address, u32 instruction) -> void {
  debugger.instruction(address, instruction);
}

auto CPU::icacheFillLine(u64 vaddr, u32 paddr) -> void {
  icache.line(vaddr).fill(paddr, *this);
}

auto CPU::rdpBusyStall(u32 cycles, f64 rdpClocks) -> u32 {
  u32 total = 0;
  if(rdp.timing.enable && vi.active() && vi.io.antialias < 2) total += viStallCycles;  //the VI's three-line fetch
  if(rdp.timing.enable && rdp.timing.pending && !rdp.command.freeze) {
    rdp.timingDelay(rdpClocks * rdp.contendScale());  //and the RDP loses the bus for the transfer
    total += cycles;
  }
  return total;
}

//result latency of a COP1 arithmetic instruction in cycles (cpu_timing v11/v14); 0 = not one
auto CPU::fpuLatency(u32 instruction) const -> u32 {
  if((instruction >> 26) != 0x11 || !(instruction >> 25 & 1)) return 0;
  u32 fmt = instruction >> 21 & 31, fn = instruction & 0x3f;
  bool dbl = fmt == 17 || fmt == 21;
  switch(fn) {
  case 0x00: case 0x01: return 4;                     //add, sub: issue 3, a dependent instruction waits 1 more (v18)
  case 0x02: return dbl ? 9 : 6;                      //mul.s 5 / mul.d 8, +1 dependent (v18; 2 with a 0.0 / 1.0 / power-of-two operand)
  case 0x03: return dbl ? 59 : 30;                    //div.s 29 / div.d 58, +1 dependent (v18; 2 with a 0.0 dividend)
  case 0x04: return dbl ? 59 : 30;                    //sqrt.s 29 / sqrt.d 58 (v18; 2 for sqrt(0.0))
  case 0x05: case 0x06: case 0x07: return 1;          //abs, mov, neg
  case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: return 5;  //round/trunc/ceil/floor
  case 0x20: case 0x21: case 0x24: case 0x25: return 5;  //cvt
  default: return fn >= 0x30 ? 1 : 0;                 //compares
  }
}

//pipeline hazards (cycles) between an instruction and the one that follows it in memory
auto CPU::hazardCycles(u32 instruction, u32 next) -> u32 {
  auto op = instruction >> 26;
  auto isLoad = [](u32 o) { return (o >= 0x20 && o <= 0x27) || o == 0x37 || o == 0x1a || o == 0x1b || o == 0x30 || o == 0x34 || o == 0x31 || o == 0x35; };
  auto isStore = [](u32 o) { return (o >= 0x28 && o <= 0x2f) || o == 0x38 || o == 0x3c || o == 0x39 || o == 0x3d; };
  u32 cycles = 0;
  if(op == 0x10 && (instruction >> 21 & 31) <= 1 && (instruction >> 11 & 31) == 9) cycles += 1;  //MFC0 Count costs 2 (Status: 1)
  if(op == 0x10 && ((instruction >> 21 & 31) == 4 || (instruction >> 21 & 31) == 5)) cycles += 1;  //MTC0/DMTC0 cost 2
  if(!next) return cycles;
  auto nop = next >> 26;
  if(isStore(op) && (isLoad(nop) || isStore(nop) || nop == 0x2f)) return cycles + 1;  //the store occupies the data cache for one more cycle
  if(!isLoad(op)) return cycles;
  if(op == 0x31 || op == 0x35) return cycles;  //LWC1/LDC1: handled by the FPU scoreboard
  u32 rt = instruction >> 16 & 31;
  if(rt == 0) return cycles;
  u32 nrs = next >> 21 & 31, nrt = next >> 16 & 31;
  bool readsRs = !(nop == 0x02 || nop == 0x03 || nop == 0x0f || nop == 0x10 || nop == 0x11 || nop == 0x12);
  bool readsRt = nop == 0x00 || isStore(nop) || (nop == 0x10 && (nrs == 4 || nrs == 5)) || (nop == 0x11 && (nrs == 4 || nrs == 5 || nrs == 6));
  if(nop == 0x00) {  //SPECIAL: JR/JALR and SLL/SRL/SRA (rs field is the shift amount) read differently
    u32 fn = next & 0x3f;
    if(fn == 0x08 || fn == 0x09) return cycles;  //measured: lw ra; jr ra costs nothing extra
    if(fn <= 0x03) readsRs = false;
  }
  if(nop == 0x01 || nop == 0x04 || nop == 0x05 || nop == 0x14 || nop == 0x15 || nop == 0x06 || nop == 0x07 || nop == 0x16 || nop == 0x17) return cycles;  //branches
  if((readsRs && nrs == rt) || (readsRt && nrt == rt)) cycles += 1;
  return cycles;
}

auto CPU::icacheBusyStall() -> u32 {
  viStallCycles = ViStallIFill;
  u32 r = rdpBusyStall(ICacheFillStall, rdp.timing.contendCpuI);
  viStallCycles = ViStallFill;
  return r;
}

//a dirty line leaves the write buffer: the bus is busy for the 16-byte write (the CPU itself paid
//WriteBufferCycles when the line was evicted)
auto CPU::postWriteBack() -> void {
  wbPending = false;
  viStallCycles = ViStallWriteBack;
  s64 now = busNow();
  busFreeAt = (busFreeAt > now ? busFreeAt : now) + (DCacheWriteBackCycles + rdpBusyStall(DCacheWriteBackStall, rdp.timing.contendCpuW)) * 2;
  viStallCycles = ViStallFill;
}

//an uncached RDRAM or RCP register store enters the write buffer (cpu_timing v18)
auto CPU::postWrite(u32 paddr) -> void {
  s64 now = busNow();
  if(wbPending) { busWait(); postWriteBack(); busWait(); now = busNow(); }   //the pending write-back drains first
  while(wbCount && wbDone[wbHead] <= now) { wbHead = wbHead + 1 & 3; wbCount--; }
  if(wbCount >= WriteBufferEntries) {                                        //full: wait for the oldest entry
    step((u32)(wbDone[wbHead] - now)); now = busNow();
    wbHead = wbHead + 1 & 3; wbCount--;
  }
  u32 cycles;
  if(paddr < 0x03f0'0000) cycles = UncachedWriteCycles + rdpBusyStall(UncachedWriteStall, rdp.timing.contendCpuU);
  else if((paddr & 0xfff0'0000) == 0x0430'0000) cycles = MiWriteCycles;
  else cycles = RcpWriteCycles;
  s64 start = busFreeAt > now ? busFreeAt : now;
  s64 done = start + cycles * 2;
  wbDone[wbHead + wbCount & 3] = done; wbCount++;
  busFreeAt = done;
}

//the recompiler's inline instruction cache fill: the stall part of the fill time
auto CPU::icacheStall() -> void {
  busWaitFill();
  if(u32 cycles = rdpBusyStall(ICacheFillStall, rdp.timing.contendCpuI)) step(cycles * 2);
}

template<bool Recompiled>
auto CPU::instructionEpilogue() -> void {
  if constexpr(!Recompiled) {
    ipu.r[0].u64 = 0;
  }
}

auto CPU::raiseCoprocessor1Exception() -> void {
  exception.coprocessor1();
}

auto CPU::power(bool reset) -> void {
  Thread::reset();

  context.endian = Context::Endian::Big;
  context.mode = Context::Mode::Kernel;
  context.bits = 64;
  for(auto& segment : context.segment) segment = Context::Segment::Unused;
  icache.power(reset);
  dcache.power(reset);
  for(auto& entry : tlb.entry) entry = {}, entry.synchronize();
  tlb.physicalAddress = 0;
  for(auto& r : ipu.r) r.u64 = 0;
  ipu.lo.u64 = 0;
  ipu.hi.u64 = 0;
  ipu.r[29].u64 = 0xffff'ffff'a400'1ff0ull;  //stack pointer
  pipeline.setPc(0xffff'ffff'bfc0'0000ull);
  scc = {};
  for(auto& r : fpu.r) r.u64 = 0;
  fpu.csr = {};
  cop2 = {};
  emuxState = {};
  fenv.setRound(float_env::toNearest);
  context.setMode();

  if constexpr(Accuracy::CPU::Recompiler) {
    auto buffer = ares::Memory::FixedAllocator::get().tryAcquire(63_MiB);
    recompiler.allocator.resize(63_MiB, bump_allocator::executable, buffer);
    recompiler.reset();
  }
}

}
