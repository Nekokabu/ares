struct Scheduler;

struct Thread {
  enum : uintmax { Second = (uintmax)-1 >> 1 };

  struct EntryPoint {
    cothread_t handle = nullptr;
    function<void ()> entryPoint;
  };

  static inline auto EntryPoints() -> vector<EntryPoint>&;
  static inline auto Enter() -> void;

  inline virtual ~Thread();

  inline auto active() const -> bool;
  inline auto handle() const -> cothread_t;
  inline auto frequency() const -> uintmax;
  inline auto scalar() const -> uintmax;
  inline auto clock() const -> uintmax;

  inline auto setHandle(cothread_t handle) -> void;
  inline auto setFrequency(double frequency) -> void;
  inline auto setScalar(uintmax scalar) -> void;
  inline auto setClock(uintmax clock) -> void;

  inline auto create(double frequency, function<void ()> entryPoint) -> void;
  inline auto destroy() -> void;

  inline auto step(uint clocks) -> void;
  inline auto synchronize() -> bool;
  inline auto synchronize(Thread&) -> void;
  inline auto synchronize(Thread&, Thread&) -> void;
  inline auto synchronize(Thread&, Thread&, Thread&) -> void;

  inline auto serialize(serializer& s) -> void;

protected:
  cothread_t _handle = nullptr;
  uintmax _frequency = 0;
  uintmax _scalar = 0;
  uintmax _clock = 0;

  friend class Scheduler;
};
