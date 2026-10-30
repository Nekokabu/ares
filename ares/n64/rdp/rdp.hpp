//Reality Display Processor

struct RDP : Thread, Memory::RCP<RDP> {
  Node::Object node;

  struct Debugger {
    //debugger.cpp
    auto load(Node::Object) -> void;
    auto command(string_view) -> void;
    auto ioDPC(bool mode, u32 address, u32 data) -> void;
    auto ioDPS(bool mode, u32 address, u32 data) -> void;

    struct Tracer {
      Node::Debugger::Tracer::Notification command;
      Node::Debugger::Tracer::Notification io;
    } tracer;
  } debugger;

  //rdp.cpp
  auto load(Node::Object) -> void;
  auto unload() -> void;

  auto main() -> void;
  auto power(bool reset) -> void;
  auto crash(const char *reason) -> void;

  //render.cpp
  auto render() -> void;
  auto noOperation() -> void;
  auto invalidOperation() -> void;
  auto unshadedTriangle() -> void;
  auto unshadedZbufferTriangle() -> void;
  auto textureTriangle() -> void;
  auto textureZbufferTriangle() -> void;
  auto shadedTriangle() -> void;
  auto shadedZbufferTriangle() -> void;
  auto shadedTextureTriangle() -> void;
  auto shadedTextureZbufferTriangle() -> void;
  auto syncLoad() -> void;
  auto syncPipe() -> void;
  auto syncTile() -> void;
  auto syncFull() -> void;
  auto setKeyGB() -> void;
  auto setKeyR() -> void;
  auto setConvert() -> void;
  auto setScissor() -> void;
  auto setPrimitiveDepth() -> void;
  auto setOtherModes() -> void;
  auto textureRectangle() -> void;
  auto textureRectangleFlip() -> void;
  auto loadTLUT() -> void;
  auto setTileSize() -> void;
  auto loadBlock() -> void;
  auto loadTile() -> void;
  auto setTile() -> void;
  auto fillRectangle() -> void;
  auto setFillColor() -> void;
  auto setFogColor() -> void;
  auto setBlendColor() -> void;
  auto setPrimitiveColor() -> void;
  auto setEnvironmentColor() -> void;
  auto setCombineMode() -> void;
  auto setTextureImage() -> void;
  auto setMaskImage() -> void;
  auto setColorImage() -> void;

  //io.cpp
  auto readWord(u32 address, Thread& thread) -> u32;
  auto writeWord(u32 address, u32 data, Thread& thread) -> void;
  auto flushCommands() -> void;

  //timing.cpp
  auto timingReset() -> void;
  auto timingCommand(const u64* ops, u32 length) -> void;
  auto timingSyncFull() -> void;
  auto timingStep(u32 clocks) -> u32;
  auto timingComplete() -> void;
  auto timingTriangle(const u64* ops, bool shade, bool texture, bool zbuffer) -> void;
  auto timingRectangle(s32 x0, s32 y0, s32 x1, s32 y1, bool fill) -> void;
  auto timingMode() -> u32;
  auto timingScale(bool memoryBound) -> f64;
  auto viModeFactor() -> f64;
  auto contendScale() -> f64;
  auto timingSpan(s32 x, s32 y, u32 pixels) -> f64;
  auto timingAdd(f64 cycles) -> void;
  auto timingDelay(f64 cycles) -> void;

  //serialization.cpp
  auto serialize(serializer&) -> void;

  //RDP command timing model: estimates how long the RDP is busy executing the
  //command stream so that pipe/buffer busy flags and the DP interrupt (Sync_Full)
  //are delayed accordingly rather than completing instantly.
  struct Timing {
    //tunables
    bool   enable = true;
    //the other bus masters are charged as they happen (timingDelay) instead of a fixed factor
    f64    contentionZ = 1.0;      //scale on read-modify-write spans (image read / depth) beyond the per-transfer charges below
    f64    contentionPlain = 1.0;  //the same for write-only spans (plain 1/2-cycle, fill, copy) and TMEM loads
    //the VI's framebuffer fetch slows every RDRAM transfer; measured by replaying game display lists under
    //every VI mode on an NTSC console (rdp_timing v12/v15): AA mode 3 (replicate, one line fetched per
    //output line) +0.4%, mode 2 (resample) +3%, modes 0/1 (anti-aliased with divot: three lines per output
    //line) +11.7% at 320x16-bit, +32% at 640x16-bit; mode 2 does not grow with the line (1280 pixels: +3%)
    f64    viFactorAA3 = 1.004, viFactorAA2 = 1.03, viFactorAA0 = 1.117;
    f64    viWidthScale = 1.77;    //AA 0/1: the factor over 1 grows by this per 640 bytes of line above 640: (1.324-1)/(1.117-1) = 1 + 1.77*(1280/640-1)
    f64    viContendScale = 1.6;   //the per-transfer losses below grow with the VI factor: 1 + 0.117*1.6 at 320x16 AA0 (v12: x1.25 at +15.5%)
    //RDP clocks lost to each transfer of another bus master while it has work in flight (rdp_timing v10, RDP drawing a game list):
    f64    contendCpu = 11.7;      //a CPU data cache line fill (16 bytes)
    f64    contendCpuI = 8.5;      //a CPU instruction cache line fill (32 bytes)
    f64    contendCpuW = 1.0;      //a CPU data cache write-back (16 bytes; rdp_timing v17: a random load+store loop costs the RDP about two fills' worth, not two fills and two write-backs)
    f64    contendCpuU = 4.0;      //an uncached CPU write (rdp_timing v17: 2.7 with the VI off, 5 in the games' VI mode)
    f64    contendDma = 1.22;      //per 8 bytes of SP DMA (64 B: 10, 512 B: 77, 4 KiB: 656)
    f64    spanPenalty = 15.0;     //extra clocks per read-modify-write span of a primitive placed anywhere but right after the previous one (rdp_timing v9: the game lists need 15 on top of the stacked-primitive tables)
    f64    zPassFraction = 0.75;   //share of depth-tested pixels that pass (estimated from overdraw when zPassAuto)
    bool   zPassAuto = true;
    u64    frameZPixels;           //depth-tested pixels drawn since the last Sync_Full

    //state decoded from the command stream (only what the model needs)
    n2     cycleType;
    n1     imageRead;
    n1     zCompare;
    n1     zUpdate;
    n2     colorSize;              //0=4bpp 1=8bpp 2=16bpp 3=32bpp
    n10    colorWidth;
    n26    colorAddress;
    n2     textureSize;
    n10    textureWidth;
    n26    textureAddress;
    s32    scissorX0, scissorY0, scissorX1, scissorY1;
    u32    mode;                   //rdp_tm_mode derived from the state above
    bool   memoryBound;            //'mode' reads the color or depth buffer for every pixel
    f64    pipeCycles;             //pixel-pipeline floor per pixel for 'mode'
    f64    accumulated;            //fractional RDP clocks not yet submitted

    //work accounting, in system clocks (3 per RDP clock)
    u64    submitted;              //total work submitted
    u64    completed;              //total work completed
    u64    pending;                //submitted - completed
    u64    syncTargets[8];         //'submitted' snapshot at each pending Sync_Full
    u32    syncQueued;             //number of Sync_Full commands whose interrupt is still pending

    //statistics for the current frame (debug/trace)
    u64    framePixels;
    u64    frameCycles;
    u64    syncFullCount;         //total Sync_Full commands seen (statistics)
  } timing;

  struct Command {
    n24 start;
    n24 end;
    n24 current;
    n24 clock;
    n24 bufferBusy;
    n24 pipeBusy;
    n24 tmemBusy;
    n1  source;  //0 = RDRAM, 1 = DMEM
    n1  freeze;
    n1  crashed;
    n1  flush;
    n1  startValid;
    n1  endValid;
    n1  startGclk;
    n1  ready = 1;
  } command;

  struct Point {
    n16 i;  //integer
    n16 f;  //fraction
  };

  struct Edge {
    n1 lmajor;
    n3 level;
    n3 tile;
    struct Y {
      n14 hi;
      n14 md;
      n14 lo;
    } y;
    struct X {
      struct {
        Point c;  //coordinate
        Point s;  //inverse slope
      } hi, md, lo;
    } x;
  } edge;

  struct Shade {
    struct Channel {
      Point c;  //color
      Point x;  //change per X coordinate
      Point y;  //change per Y coordinate
      Point e;  //change along edge
    } r, g, b, a;
  } shade;

  struct Texture {
    struct {
      Point c;  //coordinate
      Point x;  //change per X coordinate
      Point y;  //change per Y coordinate
      Point e;  //change along edge
    } s, t, w;
  } texture;

  struct Zbuffer {
    Point d;  //inverse depth
    Point x;  //change per X coordinate
    Point y;  //change per Y coordinate
    Point e;  //change along edge
  } zbuffer;

  struct TextureRectangle {
    n3 tile;
    struct {
      n12 hi;
      n12 lo;
    } x, y;
    Point s;
    Point t;
  } rectangle;

  struct OtherModes {
    n1 atomicPrimitive;
    n1 reserved1;
    n2 cycleType;
    n1 perspective;
    n1 detailTexture;
    n1 sharpenTexture;
    n1 lodTexture;
    n1 tlut;
    n1 tlutType;
    n1 sampleType;
    n1 midTexel;
    n1 bilerp[2];
    n1 convertOne;
    n1 colorKey;
    n2 colorDitherMode;
    n2 alphaDitherMode;
    n4 reserved2;
    n2 blend1a[2];
    n2 blend1b[2];
    n2 blend2a[2];
    n2 blend2b[2];
    n1 reserved3;
    n1 forceBlend;
    n1 alphaCoverage;
    n1 coverageXalpha;
    n2 zMode;
    n2 coverageMode;
    n1 colorOnCoverage;
    n1 imageRead;
    n1 zUpdate;
    n1 zCompare;
    n1 antialias;
    n1 zSource;
    n1 ditherAlpha;
    n1 alphaCompare;
  } other;

  struct FogColor {
    n8 red;
    n8 green;
    n8 blue;
    n8 alpha;
  } fog;

  struct Blend {
    n8 red;
    n8 green;
    n8 blue;
    n8 alpha;
  } blend;

  struct PrimitiveColor {
    n5 minimum;
    n8 fraction;
    n8 red;
    n8 green;
    n8 blue;
    n8 alpha;
  } primitive;

  struct EnvironmentColor {
    n8 red;
    n8 green;
    n8 blue;
    n8 alpha;
  } environment;

  struct CombineMode {
    struct MUL {
      n5 color[2];
      n3 alpha[2];
    } mul;
    struct ADD {
      n3 color[2];
      n3 alpha[2];
    } add;
    struct SUB {
      n4 color[2];
      n3 alpha[2];
    } sba, sbb;
  } combine;

  struct TLUT {
    n3 index;
    struct {
      n12 lo;
      n12 hi;
    } s, t;
  } tlut;

  struct Load {
    struct Block {
      n3 index;
      struct {
        n12 lo;
        n12 hi;
      } s, t;
    } block;
    struct Tile {
      n3 index;
      struct {
        n12 lo;
        n12 hi;
      } s, t;
    } tile;
  } load_;

  struct TileSize {
    n3 index;
    struct {
      n12 lo;
      n12 hi;
    } s, t;
  } tileSize;

  struct Tile {
    n3 format;
    n2 size;
    n9 line;
    n9 address;
    n3 index;
    n4 palette;
    struct {
      n1 clamp;
      n1 mirror;
      n4 mask;
      n4 shift;
    } s, t;
  } tile;

  struct Set {
    struct Fill {
      n32 color = 0;
    } fill;
    struct Texture {
      n3  format = 0;
      n2  size = 0;
      n10 width = 0;
      n26 dramAddress = 0;
    } texture;
    struct Mask {
      n26 dramAddress = 0;
    } mask;
    struct Color {
      n3  format = 0;
      n2  size = 0;
      n10 width = 0;
      n26 dramAddress = 0;
    } color;
  } set;

  struct PrimitiveDepth {
    n16 z;
    n16 deltaZ;
  } primitiveDepth;

  struct Scissor {
    n1 field;
    n1 odd;
    struct {
      n12 lo;
      n12 hi;
    } x, y;
  } scissor;

  struct Convert {
    n9 k[6];
  } convert;

  struct Key {
    struct {
      n12 width;
      n8  center;
      n8  scale;
    } r, g, b;
  } key;

  struct FillRectangle {
    struct {
      n12 lo;
      n12 hi;
    } x, y;
  } fillRectangle_;

  struct IO : Memory::RCP<IO> {
    RDP& self;
    IO(RDP& self) : self(self) {}

    //io.cpp
    auto readWord(u32 address, Thread& thread) -> u32;
    auto writeWord(u32 address, u32 data, Thread& thread) -> void;

    struct BIST {
      n1 check;
      n1 go;
      n1 done;
      n8 fail;
    } bist;
    struct Test {
      n1  enable;
      n7  address;
      array<u32[128]> data;
    } test;

  } io{*this};

  n1 mapIdentityWarned;
};

extern RDP rdp;
