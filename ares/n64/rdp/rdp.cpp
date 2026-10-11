#include <n64/n64.hpp>
#include "timing-model.hpp"  //measured RDP command costs (tables)

namespace ares::Nintendo64 {

RDP rdp;
#include "render.cpp"
#include "timing.cpp"
#include "io.cpp"
#include "debugger.cpp"
#include "serialization.cpp"

auto RDP::load(Node::Object parent) -> void {
  node = parent->append<Node::Object>("RDP");
  debugger.load(node);
}

auto RDP::unload() -> void {
  debugger = {};
  node.reset();
}

auto RDP::crash(const char *reason) -> void {
  debug(unusual, "[RDP] software triggered a hardware bug; RDP crashed and will stop responding. Reason: ", reason);
  command.crashed = 1;
  //guard against asynchronous reporting of crash state. We want the RDP to report that it's busy forever
  command.pipeBusy = 1;
  command.bufferBusy = 1;
  timing.pending = 0;
  timing.syncQueued = 0;
}

auto RDP::main() -> void {
  while(Thread::clock < 0) {
    //advance in bounded chunks so that queued work completes at the right time
    u32 clocks = (u32)min<s64>(-Thread::clock, 1 << 24);
    if(timing.pending && !command.freeze && !command.crashed) {
      clocks = timingStep(clocks);
    }
    step(clocks);
    command.clock += clocks / 3;
  }
}

auto RDP::power(bool reset) -> void {
  Thread::reset();
  command = {};
  edge = {};
  shade = {};
  texture = {};
  zbuffer = {};
  rectangle = {};
  other = {};
  fog = {};
  blend = {};
  primitive = {};
  environment = {};
  combine = {};
  tlut = {};
  load_ = {};
  tileSize = {};
  tile = {};
  set = {};
  primitiveDepth = {};
  scissor = {};
  convert = {};
  key = {};
  fillRectangle_ = {};
  io.bist = {};
  io.test = {};
  timingReset();
  if(!reset) mapIdentityWarned = 0;
}

}
