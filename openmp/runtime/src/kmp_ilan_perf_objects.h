#pragma once
#include <cstdint>

enum EventCodes : uint64_t {
  TOT_DISPATCH_SLOTS = 0x430076,
  BACKEND_BOUND = 0x100431EA0,
  BACKEND_MEM_NUMERATOR = 0x43A2D6,
  BACKEND_MEM_DENOMINATOR = 0x4302D6,
  RETIRING = 0x4300C1,
  L1CACHE_FILL_DIFFERENT_NUMA = 0x435044,
  L1CACHE_FILL_SAME_CXX = 0x430344,
  L1CACHE_FILL_ANOTHER_CXX = 0x431444,
  L1CACHE_FILL_ALL = 0x435F44,
  L3_MISS = 0x040104
  // L3_MISS = 0x0300C00000400104
};

struct AMDRawResults {
  explicit AMDRawResults()
      : m_totDisp(0), m_l1All(0), m_l1DiffNuma(0), m_l1SameCXX(0),
        m_l1AnotherCXX(0), m_l3Miss(0), m_backend(0), m_backendMem(0),
        m_backendCPU(0), m_retiring(0) {};
  explicit AMDRawResults(uint64_t disp, uint64_t l1All, uint64_t l1DiffNuma,
                         uint64_t l1SameCXX, uint64_t l1AnotherCXX,
                         uint64_t l3Miss, double bound, double boundMem,
                         double boundCPU, double retiring)
      : m_totDisp(disp), m_l1All(l1All), m_l1DiffNuma(l1DiffNuma),
        m_l1SameCXX(l1SameCXX), m_l1AnotherCXX(l1AnotherCXX), m_l3Miss(l3Miss),
        m_backend(bound), m_backendMem(boundMem), m_backendCPU(boundCPU),
        m_retiring(retiring) {};
  AMDRawResults &operator+=(const AMDRawResults &other);
  AMDRawResults avg(uint32_t nthreads) const;

  uint64_t m_totDisp;
  uint64_t m_l1All;
  uint64_t m_l1DiffNuma;
  uint64_t m_l1SameCXX;
  uint64_t m_l1AnotherCXX;
  uint64_t m_l3Miss;
  double m_backend;
  double m_backendMem;
  double m_backendCPU;
  double m_retiring;
};

///////////////////////////////
/// @brief Raw event slots  ///
///////////////////////////////
template <EventCodes code> class AMDRawEvent {
public:
  explicit AMDRawEvent(int32_t cpu_id);

  void initCounter();
  void startCounter() const;
  uint64_t stopAndRead();
  void disableCounter();
  void resetAccumulatedCounter();
  uint64_t accumulatedCounter();

private:
  int32_t m_fd;
  int32_t m_cpu;
  uint64_t m_accumCounter;
};

///////////////////////////////////////////////////
/// @brief Container for all AMD perf counters  ///
///////////////////////////////////////////////////
class RawAMDPerfContainer {
public:
  explicit RawAMDPerfContainer(int32_t cpu_id, int32_t gtid);
  RawAMDPerfContainer &operator=(RawAMDPerfContainer &&other);
  RawAMDPerfContainer &operator=(const RawAMDPerfContainer &other) = delete;
  ~RawAMDPerfContainer() = default;

  void initAll();
  void startAll() const;
  AMDRawResults stopAndReadAll();
  AMDRawResults summarizeCounters();
  void disableAll();

private:
  int32_t m_cpu;
  int32_t m_gtid;
  AMDRawEvent<EventCodes::TOT_DISPATCH_SLOTS> m_totDisp;
  AMDRawEvent<EventCodes::BACKEND_BOUND> m_backendBound;
  AMDRawEvent<EventCodes::BACKEND_MEM_NUMERATOR> m_backendMemNumerator;
  AMDRawEvent<EventCodes::BACKEND_MEM_DENOMINATOR> m_backendMemDenominator;
  AMDRawEvent<EventCodes::RETIRING> m_retiring;
  AMDRawEvent<EventCodes::L1CACHE_FILL_DIFFERENT_NUMA> m_l1FillDifferentNuma;
  AMDRawEvent<EventCodes::L1CACHE_FILL_SAME_CXX> m_l1FillSameCXX;
  AMDRawEvent<EventCodes::L1CACHE_FILL_ANOTHER_CXX> m_l1FillAnotherCXX;
  AMDRawEvent<EventCodes::L1CACHE_FILL_ALL> m_l1FillAll;
  AMDRawEvent<EventCodes::L3_MISS> m_l3Miss;
};
