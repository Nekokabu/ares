//RDP execution time model
//
//ares does not emulate the RDP rasterizer at the cycle level: parallel-rdp (or the stub
//software path) consumes a whole command list the moment DPC_END is written, and the
//Sync_Full interrupt used to be raised immediately. On hardware the RDP takes a
//significant amount of time per frame. Games that measure the number of VI retraces
//between frames and scale their simulation by it behave differently when the DP
//interrupt arrives too early. A prominent example is the attract-mode demo of
//Donkey Kong 64, which replays recorded inputs frame by frame: the hardware runs the
//scene at a steady three retraces per frame, and the recording only makes sense at that
//rate. Without RDP timing the frame rate hovers around the 2/3 retrace boundary, the
//frame delta flips between 2 and 3, and DK falls into the river instead of grabbing
//the vine.
//
//This model walks the same command stream the renderer consumes and charges every
//command the number of RDP clocks (62.5MHz) it was measured to take on a console
//(timing-model.hpp; rdp_timing test ROMs v3-v8 on NTSC and PAL units, read back from
//DPC_CLOCK):
//  * a rectangle or triangle costs a per-primitive overhead plus the cost of each of
//    its spans (scanlines). The span cost is a table lookup by mode (cycle type, color
//    depth, image read, depth compare/update), by the position of the first pixel
//    within its 64-byte RDRAM block, and by the span width; it already includes the
//    color/depth buffer traffic of the RDP's memory interface and its row misses
//  * TMEM loads (Load_Block/Load_Tile/Load_TLUT) cost a base plus per-word and
//    per-64-byte-block terms, Sync_Pipe/Sync_Tile/Sync_Load 50/33/25 clocks, every other
//    command 2.62 clocks (1.73 when the list is read from DMEM)
//The tables were measured with primitives stacked one after another on an otherwise
//idle console. Three measured corrections turn them into the cost in a running game:
//  * a primitive of a real scene lands somewhere other than right after the previous
//    one; every span of a primitive that reads the color or depth buffer then costs
//    'spanPenalty' more (13-26 clocks for triangle strips, 30-50 for scattered
//    primitives; write-only spans: none)
//  * the CPU, the RSP and the VI compete for RDRAM: read-modify-write spans are
//    'contentionZ' (1.13) slower at the traffic of a running game and 'viFactor' (1.07)
//    slower while the VI scans out a frame; write-only spans and TMEM loads are
//    'contentionPlain' (1.19) slower and do not care about the VI
//  * depth-tested spans cost less when the depth test fails (no color/depth write), and
//    the tables hold both cases; the share of passing pixels is estimated from the
//    overdraw of the previous frame
//The model was checked against 60fps HDMI captures of a console: the Donkey Kong 64
//attract demo (retraces per displayed frame over the demo, and the demo playing back
//correctly) and the Beetle Adventure Racing intro (frames per scene and the timing of
//the helicopter crash). The CPU side of the same measurements (cache fills waiting for
//the bus while the RDP draws) lives in cpu/.
//
//The resulting work is consumed by RDP::main() as the RDP thread advances (and paused
//while the RDP is frozen); the DP interrupt for a Sync_Full is raised, and the busy
//flags in DPC_STATUS cleared, once all work queued before it has completed.

auto RDP::timingReset() -> void {
  auto t = timing;  //preserve tunables
  timing = {};
  timing.enable = t.enable;
  timing.contentionZ = t.contentionZ;
  timing.contentionPlain = t.contentionPlain;
  timing.spanPenalty = t.spanPenalty;
  timing.contendCpu = t.contendCpu; timing.contendCpuI = t.contendCpuI; timing.contendCpuW = t.contendCpuW; timing.contendCpuU = t.contendCpuU;
  timing.viFactorAA3 = t.viFactorAA3; timing.viFactorAA2 = t.viFactorAA2; timing.viFactorAA0 = t.viFactorAA0;
  timing.viWidthScale = t.viWidthScale; timing.viContendScale = t.viContendScale;
  timing.contendDma = t.contendDma; timing.contendDmaBase = t.contendDmaBase;
  timing.zPassFraction = t.zPassFraction;
  timing.zPassAuto = t.zPassAuto;
  timing.colorSize = 2;
  timing.colorWidth = 319;
  timing.textureSize = 2;
  timing.textureWidth = 0;
  timingMode();
}

//queue 'cycles' RDP clocks of work (3 system clocks per RDP clock)
auto RDP::timingAdd(f64 cycles) -> void {
  if(!timing.enable || cycles <= 0.0) return;
  timing.frameCycles += (u64)cycles;
  timing.accumulated += cycles * 3.0;
  u64 work = (u64)timing.accumulated;
  timing.accumulated -= work;
  timing.submitted += work;
  timing.pending += work;
}

//another bus master held RDRAM for 'cycles' RDP clocks while the RDP had work in flight:
//everything queued, including the Sync_Full interrupts already waiting, completes that much later
auto RDP::timingDelay(f64 cycles) -> void {
  if(!timing.enable || cycles <= 0.0 || timing.pending == 0 || command.freeze) return;
  timing.accumulated += cycles * 3.0;
  u64 work = (u64)timing.accumulated;
  timing.accumulated -= work;
  timing.submitted += work;
  timing.pending += work;
  for(u32 n : range(timing.syncQueued)) timing.syncTargets[n] += work;
}

//pick the table row for the current Set_Other_Modes / Set_Color_Image state
auto RDP::timingMode() -> u32 {
  bool c32 = timing.colorSize == 3;
  u32 bytes = c32 ? 4 : timing.colorSize == 2 ? 2 : 1;
  u32 mode;
  switch(timing.cycleType) {
  case 3:  //fill: 64 bits per clock
    mode = c32 ? RDP_TM_FILL32 : RDP_TM_FILL16;
    timing.pipeCycles = bytes / 8.0;
    break;
  case 2:  //copy: 64 bits per clock
    mode = RDP_TM_COPY16;
    timing.pipeCycles = bytes / 8.0;
    break;
  case 1:  //2-cycle: one pixel per two clocks
    mode = c32 ? RDP_TM_2CYC32 : RDP_TM_2CYC16;
    timing.pipeCycles = 2.0;
    break;
  default: //1-cycle: one pixel per clock
    mode = c32 ? RDP_TM_1CYC32 : RDP_TM_1CYC16;
    timing.pipeCycles = 1.0;
    break;
  }
  if(timing.cycleType < 2) {
    bool z = timing.zCompare && timing.zUpdate;
    if(timing.imageRead && z) {
      mode = timing.cycleType ? (c32 ? RDP_TM_2CYC32_BLEND_ZPASS : RDP_TM_2CYC16_BLEND_ZPASS)
                              : (c32 ? RDP_TM_1CYC32_BLEND_ZPASS : RDP_TM_1CYC16_BLEND_ZPASS);
    } else if(timing.imageRead) {
      mode = timing.cycleType ? (c32 ? RDP_TM_2CYC32_BLEND : RDP_TM_2CYC16_BLEND)
                              : (c32 ? RDP_TM_1CYC32_BLEND : RDP_TM_1CYC16_BLEND);
    } else if(z || timing.zCompare || timing.zUpdate) {
      mode = timing.cycleType ? (c32 ? RDP_TM_2CYC32_ZPASS : RDP_TM_2CYC16_ZPASS)
                              : (c32 ? RDP_TM_1CYC32_ZPASS : RDP_TM_1CYC16_ZPASS);
    }
  }
  timing.memoryBound = timing.cycleType < 2 && (timing.imageRead || timing.zCompare || timing.zUpdate);
  return timing.mode = mode;
}

//how much the VI's framebuffer fetch slows the RDP in the current VI mode (see rdp.hpp)
auto RDP::viModeFactor() -> f64 {
  if(!vi.active()) return 1.0;
  f64 f = vi.io.antialias == 3 ? timing.viFactorAA3 : vi.io.antialias == 2 ? timing.viFactorAA2 : timing.viFactorAA0;
  u32 bytes = ((u32)vi.io.width ? (u32)vi.io.width : 320u) * (vi.io.colorDepth == 3 ? 4u : 2u);
  if(bytes > 640 && vi.io.antialias < 2) f = 1.0 + (f - 1.0) * (1.0 + timing.viWidthScale * ((f64)bytes / 640.0 - 1.0));
  return f;
}

//what a span or load costs in a running game relative to the idle-console tables
auto RDP::timingScale(bool memoryBound) -> f64 {
  f64 f = memoryBound ? timing.contentionZ : timing.contentionPlain;
  return f * viModeFactor();
}

//what a bus transfer of another master costs the RDP, scaled by the VI's own traffic (v12: x1.25 in the AA modes)
auto RDP::contendScale() -> f64 {
  return 1.0 + (viModeFactor() - 1.0) * timing.viContendScale;
}

//cost of one span of 'pixels' pixels whose first pixel is at (x, y) of the color image
auto RDP::timingSpan(s32 x, s32 y, u32 pixels) -> f64 {
  if(pixels == 0) return 0.0;
  u32 bytes = timing.colorSize == 3 ? 4 : timing.colorSize == 2 ? 2 : 1;
  u32 stride = (timing.colorWidth + 1) * bytes;
  u32 address = timing.colorAddress + y * stride + x * bytes;
  auto mode = (enum rdp_tm_mode)timing.mode;
  s32 w = pixels;
  if(bytes == 1) {
    //4/8-bit color images were not measured: use the 16-bit tables at the same byte count
    address = timing.colorAddress + y * stride + (x >> 1) * 2;
    w = (pixels + 1) >> 1;
  }
  if(timing.cycleType == 2 && bytes == 4) w = pixels * 2;  //copy mode was measured at 16-bit only
  if(w > 4096) w = 4096;

  f64 cost = rdp_tm_span(mode, address, w);
  if(timing.cycleType < 2 && (timing.zCompare || timing.zUpdate)) {
    //the tables hold "every pixel passes" (zpass) and "every pixel fails" (zfail);
    //blend the two by the assumed pass rate. Compare-only and update-only were
    //measured for 1-cycle 16-bit; scale that row for the other modes.
    bool c32 = timing.colorSize == 3;
    bool both = timing.zCompare && timing.zUpdate;
    auto zpass = timing.cycleType ? (c32 ? RDP_TM_2CYC32_ZPASS : RDP_TM_2CYC16_ZPASS)
                                  : (c32 ? RDP_TM_1CYC32_ZPASS : RDP_TM_1CYC16_ZPASS);
    auto zfail = timing.cycleType ? (c32 ? RDP_TM_2CYC32_ZFAIL : RDP_TM_2CYC16_ZFAIL)
                                  : (c32 ? RDP_TM_1CYC32_ZFAIL : RDP_TM_1CYC16_ZFAIL);
    f64 pass = rdp_tm_span(zpass, address, w);
    f64 fail = rdp_tm_span(zfail, address, w);
    timing.frameZPixels += pixels;
    if(both) {
      cost += (fail - pass) * (1.0 - timing.zPassFraction);
    } else {
      auto partial = timing.zCompare ? RDP_TM_1CYC16_ZCMP : RDP_TM_1CYC16_ZUPD;
      f64 ref = rdp_tm_span(RDP_TM_1CYC16_ZPASS, address, w);
      f64 scaled = rdp_tm_span(partial, address, w) * (ref > 0.0 ? pass / ref : 1.0);
      cost += scaled - pass;
    }
  }

  //other bus masters (CPU, RSP, VI) lengthen every span: measured as a factor on the span
  f64 pipe = pixels * timing.pipeCycles;
  if(cost < pipe) cost = pipe;
  cost *= timingScale(timing.memoryBound);
  if(timing.memoryBound) cost += timing.spanPenalty;

  timing.framePixels += pixels;
  return cost;
}

auto RDP::timingTriangle(const u64* ops, bool shade, bool texture, bool zbuffer) -> void {
  //edge coefficients: Y in 11.2 signed fixed point, X in 16.16 signed fixed point
  auto s14 = [](u64 v) -> s32 { s32 r = v & 0x3fff; if(r & 0x2000) r -= 0x4000; return r; };
  s32 yl = s14(ops[0] >> 32);
  s32 ym = s14(ops[0] >> 16);
  s32 yh = s14(ops[0] >>  0);
  s32 xl    = (s32)(u32)(ops[1] >> 32), dxldy = (s32)(u32)(ops[1] >> 0);
  s32 xh    = (s32)(u32)(ops[2] >> 32), dxhdy = (s32)(u32)(ops[2] >> 0);
  s32 xm    = (s32)(u32)(ops[3] >> 32), dxmdy = (s32)(u32)(ops[3] >> 0);

  f64 fyh = yh / 4.0, fym = ym / 4.0, fyl = yl / 4.0;
  f64 fxh = xh / 65536.0, fdxhdy = dxhdy / 65536.0;
  f64 fxm = xm / 65536.0, fdxmdy = dxmdy / 65536.0;
  f64 fxl = xl / 65536.0, fdxldy = dxldy / 65536.0;

  s32 rowStart = (s32)floor(fyh);
  s32 rowEnd   = (s32)ceil(fyl);
  if(rowStart < timing.scissorY0) rowStart = timing.scissorY0;
  if(rowEnd   > timing.scissorY1) rowEnd   = timing.scissorY1;
  if(rowEnd - rowStart > 1024) rowEnd = rowStart + 1024;  //guard against garbage

  f64 cycles = 0.0;
  u32 widest = 0;
  for(s32 row = rowStart; row < rowEnd; row++) {
    f64 yc = row + 0.5;
    if(yc < fyh || yc >= fyl) continue;
    f64 xMajor = fxh + fdxhdy * (yc - fyh);
    f64 xMinor = yc < fym ? fxm + fdxmdy * (yc - fyh) : fxl + fdxldy * (yc - fym);
    f64 x0 = xMajor < xMinor ? xMajor : xMinor;
    f64 x1 = xMajor < xMinor ? xMinor : xMajor;
    if(x0 < timing.scissorX0) x0 = timing.scissorX0;
    if(x1 > timing.scissorX1) x1 = timing.scissorX1;
    if(x1 <= x0) continue;
    u32 pixels = (u32)(x1 - x0 + 0.5);
    if(pixels == 0) pixels = 1;
    if(pixels > widest) widest = pixels;
    cycles += timingSpan((s32)floor(x0), row, pixels);
  }
  //per-primitive overhead (can be slightly negative: setup overlaps the previous primitive)
  cycles += rdp_tm_prim_overhead((enum rdp_tm_mode)timing.mode, widest ? widest : 1);
  if(shade || texture) cycles += 3.0;
  timingAdd(cycles);
}

auto RDP::timingRectangle(s32 x0, s32 y0, s32 x1, s32 y1, bool fill) -> void {
  //coordinates in 10.2 fixed point; fill/copy rectangles are inclusive of the end coordinate
  s32 px0 = x0 >> 2, px1 = (x1 >> 2) + (timing.cycleType >= 2 ? 1 : 0);
  s32 py0 = y0 >> 2, py1 = (y1 >> 2) + (timing.cycleType >= 2 ? 1 : 0);
  if(px0 < timing.scissorX0) px0 = timing.scissorX0;
  if(py0 < timing.scissorY0) py0 = timing.scissorY0;
  if(px1 > timing.scissorX1) px1 = timing.scissorX1;
  if(py1 > timing.scissorY1) py1 = timing.scissorY1;
  f64 cycles = 0.0;
  u32 width = px1 > px0 ? px1 - px0 : 0;
  if(width && py1 > py0) {
    for(s32 row = py0; row < py1; row++) cycles += timingSpan(px0, row, width);
  }
  cycles += rdp_tm_prim_overhead((enum rdp_tm_mode)timing.mode, width ? width : 1);
  timingAdd(cycles);
}

auto RDP::timingSyncFull() -> void {
  if(!timing.enable) return;
  timing.syncFullCount++;
  timingAdd(RDP_TM_LIST_BASE);  //list start + Sync_Full
  if(timing.syncQueued == 8) {
    //should never happen (games wait for the DP interrupt); complete the oldest one
    timing.syncQueued--;
    for(u32 n : range(timing.syncQueued)) timing.syncTargets[n] = timing.syncTargets[n + 1];
    timingComplete();
  }
  timing.syncTargets[timing.syncQueued++] = timing.submitted;
}

//advance the RDP by up to 'clocks' system clocks of queued work; returns the clocks consumed
auto RDP::timingStep(u32 clocks) -> u32 {
  if(timing.pending == 0) return 0;
  if((u64)clocks > timing.pending) clocks = (u32)timing.pending;
  timing.pending -= clocks;
  timing.completed += clocks;
  while(timing.syncQueued && timing.completed >= timing.syncTargets[0]) {
    timing.syncQueued--;
    for(u32 n : range(timing.syncQueued)) timing.syncTargets[n] = timing.syncTargets[n + 1];
    timingComplete();
  }
  return clocks;
}

//a Sync_Full has finished executing: signal the CPU
auto RDP::timingComplete() -> void {
  if(!command.crashed) {
    mi.raise(MI::IRQ::DP);
    if(timing.pending == 0) {
      command.bufferBusy = 0;
      command.pipeBusy = 0;
      command.startGclk = 0;
    }
  }
  if(timing.zPassAuto) {
    //estimate the share of depth-tested pixels that pass from the overdraw of the frame just
    //finished: drawing n layers in random order, the k-th layer passes with probability 1/k,
    //so the fraction is H(n)/n (1.0 for a single layer, 0.75 for two, ~0.6 for three)
    f64 area = (f64)(timing.scissorX1 - timing.scissorX0) * (timing.scissorY1 - timing.scissorY0);
    if(area > 0 && timing.frameZPixels > 0) {
      f64 n = timing.frameZPixels / area;
      if(n < 1.0) n = 1.0;
      f64 h = log(n) + 0.5772 + 0.5 / n;  //harmonic number
      f64 f = h / n;
      if(f > 1.0) f = 1.0;
      if(f < 0.5) f = 0.5;
      timing.zPassFraction = f;
    }
  }
  timing.frameZPixels = 0;
  timing.framePixels = 0;
  timing.frameCycles = 0;
}

//account for one complete RDP command (ops[0] holds the first 64-bit word)
auto RDP::timingCommand(const u64* ops, u32 length) -> void {
  if(!timing.enable) return;
  u64 op = ops[0];
  u32 code = op >> 56 & 0x3f;
  const f64 state = rdp_tm_state_cmd(command.source);

  //bytes per texel of the texture image, times two (so that 4bpp is 1)
  auto texelBytes2 = [&](u32 texels) -> u32 {
    return timing.textureSize == 3 ? texels * 8 : timing.textureSize == 2 ? texels * 4 : timing.textureSize == 1 ? texels * 2 : texels;
  };

  switch(code) {
  case 0x08: timingTriangle(ops, 0, 0, 0); break;
  case 0x09: timingTriangle(ops, 0, 0, 1); break;
  case 0x0a: timingTriangle(ops, 0, 1, 0); break;
  case 0x0b: timingTriangle(ops, 0, 1, 1); break;
  case 0x0c: timingTriangle(ops, 1, 0, 0); break;
  case 0x0d: timingTriangle(ops, 1, 0, 1); break;
  case 0x0e: timingTriangle(ops, 1, 1, 0); break;
  case 0x0f: timingTriangle(ops, 1, 1, 1); break;

  case 0x24:   //Texture_Rectangle
  case 0x25: { //Texture_Rectangle_Flip
    s32 x1 = n12(op >> 44), y1 = n12(op >> 32);
    s32 x0 = n12(op >> 12), y0 = n12(op >>  0);
    timingRectangle(x0, y0, x1, y1, false);
  } break;

  case 0x26:   //Sync_Load
  case 0x27:   //Sync_Pipe
  case 0x28:   //Sync_Tile
    timingAdd(rdp_tm_sync(code));
    break;
  case 0x29: timingSyncFull(); break;  //Sync_Full

  case 0x2d: { //Set_Scissor: XH/YH (upper-left) in the high bits, XL/YL (lower-right) in the low bits
    timing.scissorX0 = (s32)n12(op >> 44) >> 2;
    timing.scissorY0 = (s32)n12(op >> 32) >> 2;
    timing.scissorX1 = ((s32)n12(op >> 12) + 3) >> 2;
    timing.scissorY1 = ((s32)n12(op >>  0) + 3) >> 2;
    timingAdd(state);
  } break;

  case 0x2f: { //Set_Other_Modes
    timing.cycleType = n2(op >> 52);
    timing.imageRead = n1(op >>  6);
    timing.zUpdate   = n1(op >>  5);
    timing.zCompare  = n1(op >>  4);
    timingMode();
    timingAdd(state);
  } break;

  case 0x30: { //Load_TLUT
    s32 s0 = n12(op >> 44) >> 2, s1 = n12(op >> 12) >> 2;
    u32 count = s1 >= s0 ? s1 - s0 + 1 : 0;
    timingAdd(rdp_tm_load_tlut(count) * timingScale(false));
  } break;

  case 0x33: { //Load_Block
    s32 s0 = n12(op >> 44), s1 = n12(op >> 12);
    u32 texels = s1 >= s0 ? s1 - s0 + 1 : 0;
    u32 bytes = texelBytes2(texels) >> 1;
    timingAdd(rdp_tm_load_block((bytes + 1) >> 1) * timingScale(false));
  } break;

  case 0x34: { //Load_Tile
    s32 s0 = n12(op >> 44) >> 2, t0 = n12(op >> 32) >> 2;
    s32 s1 = n12(op >> 12) >> 2, t1 = n12(op >>  0) >> 2;
    u32 w = s1 >= s0 ? s1 - s0 + 1 : 0;
    u32 h = t1 >= t0 ? t1 - t0 + 1 : 0;
    u32 stride   = texelBytes2(timing.textureWidth + 1) >> 1;
    u32 rowBytes = texelBytes2(w) >> 1;
    u32 address  = timing.textureAddress + t0 * stride + (texelBytes2(s0) >> 1);
    if(h > 1024) h = 1024;  //guard against garbage
    timingAdd(rdp_tm_load_tile(address, stride, rowBytes, h) * timingScale(false));
  } break;

  case 0x36: { //Fill_Rectangle
    s32 x1 = n12(op >> 44), y1 = n12(op >> 32);
    s32 x0 = n12(op >> 12), y0 = n12(op >>  0);
    timingRectangle(x0, y0, x1, y1, true);
  } break;

  case 0x3d: { //Set_Texture_Image
    timing.textureSize    = n2(op >> 51);
    timing.textureWidth   = n10(op >> 32);
    timing.textureAddress = n26(op >> 0);
    timingAdd(state);
  } break;

  case 0x3f: { //Set_Color_Image
    timing.colorSize    = n2(op >> 51);
    timing.colorWidth   = n10(op >> 32);
    timing.colorAddress = n26(op >> 0);
    timingMode();
    timingAdd(state);
  } break;

  default:
    timingAdd(state);
    break;
  }
}
