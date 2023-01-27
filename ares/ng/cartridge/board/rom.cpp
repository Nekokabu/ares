struct Rom : Interface {
  using Interface::Interface;

  Memory::Readable<n16> prom;
  Memory::Readable<n8 > mrom;
  Memory::Readable<n8 > crom;
  Memory::Readable<n8 > srom;
  Memory::Readable<n8 > vromA;
  Memory::Readable<n8 > vromB;

  auto load() -> void override {
    Interface::load(prom, "program.rom");
    Interface::load(mrom, "music.rom");;
    Interface::load(crom, "character.rom");
    Interface::load(srom, "static.rom");
    Interface::load(vromA, "voice-a.rom");
    Interface::load(vromB, "voice-b.rom");
  }

  auto unload() -> void override {
    prom.reset();
    mrom.reset();
    crom.reset();
    srom.reset();
    vromA.reset();
    vromB.reset();
  }

  auto readP(n1 upper, n1 lower, n24 address, n16 data) -> n16 override {
    if(address <= 0x0fffff) return prom[address >> 1];
    if(address >= 0x200000 && address <= 0x2fffff) {
      address = ((romBank + 1) * 0x100000) | n20(address);
      return prom[address >> 1];
    }
    return data;
  }

  auto writeP(n1 upper, n1 lower, n24 address, n16 data) -> void override {
    if(lower && address >= 0x200000 && address <= 0x2fffff)  {
      romBank = data.bit(0, 2);
    }
  }

  auto readM(n32 address) -> n8 override {
    return mrom[address];
  }

  auto readC(n32 address) -> n8 override {
    return crom[address];
  }

  auto readS(n32 address) -> n8 override {
    return srom[address];
  }

  auto readVA(n32 address) -> n8 override {
    return vromA[address];
  }

  auto readVB(n32 address) -> n8 override {
    return vromB ? vromB[address] : vromA[address];
  }

  auto power() -> void override {
    romBank = 0;
  }

  n8 romBank;
};

