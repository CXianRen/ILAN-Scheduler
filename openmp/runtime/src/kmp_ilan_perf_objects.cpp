#include "kmp_ilan_perf_objects.h"

#include "kmp.h"
#include "kmp_debug.h"

#include <asm/unistd_64.h>
#include <cstdint>
#include <linux/perf_event.h>
#include <unistd.h>
#include <sys/ioctl.h>

namespace {
inline void init_attr(perf_event_attr *pe) {
  memset(pe, 0, sizeof(perf_event_attr));
  pe->size = sizeof(perf_event_attr);
  pe->disabled = 1;
  pe->exclude_kernel = 1;
  pe->exclude_hv = 1;
  pe->inherit = 0;
}

inline double frac(uint64_t numerator, uint64_t denominator) {
  if (denominator == 0) {
    return 0.0;
  }
  return static_cast<double>(numerator) / static_cast<double>(denominator);
}

int32_t perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu,
                        int group_fd, unsigned long flags) {
  return static_cast<int32_t>(
      syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
}

const char *enumToString(EventCodes event) {
  switch (event) {
  case EventCodes::TOT_DISPATCH_SLOTS:
    return "TotDisp";
  case EventCodes::BACKEND_BOUND:
    return "BackendBound";
  case EventCodes::BACKEND_MEM_NUMERATOR:
    return "BackendNumerator";
  case EventCodes::BACKEND_MEM_DENOMINATOR:
    return "BackendDenominator";
  case EventCodes::RETIRING:
    return "RETIRING";
  case EventCodes::L1CACHE_FILL_ALL:
    return "L1CACHE_FILL_ALL";
  case EventCodes::L1CACHE_FILL_DIFFERENT_NUMA:
    return "L1CACHE_FILL_DIFFERENT_NUMA";
  case EventCodes::L1CACHE_FILL_SAME_CXX:
    return "L1CACHE_FILL_SAME_CXX";
  case EventCodes::L1CACHE_FILL_ANOTHER_CXX:
    return "L1CACHE_FILL_ANOTHER_CXX";
  case EventCodes::L3_MISS:
    return "L3_MISS";
  default:
    return "UNKNOWN";
  }
}

} // namespace

AMDRawResults &AMDRawResults::operator+=(const AMDRawResults &other) {
  m_totDisp += other.m_totDisp;
  m_l1All += other.m_l1All;
  m_l1DiffNuma += other.m_l1DiffNuma;
  m_l1SameCXX += other.m_l1SameCXX;
  m_l1AnotherCXX += other.m_l1AnotherCXX;
  m_l3Miss += m_l3Miss;
  m_backend += other.m_backend;
  m_backendMem += other.m_backendMem;
  m_backendCPU += other.m_backendCPU;
  m_retiring += other.m_retiring;
  return *this;
}

AMDRawResults AMDRawResults::avg(uint32_t nthreads) const {
  return AMDRawResults(m_totDisp / nthreads, m_l1All / nthreads,
                       m_l1DiffNuma / nthreads, m_l1SameCXX / nthreads,
                       m_l1AnotherCXX / nthreads, m_l3Miss / nthreads,
                       m_backend / nthreads, m_backendMem / nthreads,
                       m_backendCPU / nthreads, m_retiring / nthreads);
}

template <EventCodes E>
AMDRawEvent<E>::AMDRawEvent(int32_t cpu_id)
    : m_fd(-1), m_cpu(cpu_id), m_accumCounter(0) {}

template <EventCodes E> void AMDRawEvent<E>::initCounter() {
  KA_TRACE(5, ("%s::initCounter: CPU#%d\n", enumToString(E), m_cpu));
  perf_event_attr pe{};
  init_attr(&pe);
  pe.type = PERF_TYPE_RAW;
  pe.config = E;
  m_fd = perf_event_open(&pe, 0, m_cpu, -1, 0);
  KMP_DEBUG_ASSERT2(m_fd > 2, "Open perf event failed");
}

template <EventCodes E> void AMDRawEvent<E>::startCounter() const {
  KA_TRACE(5, ("%s::startCounter: fd=%d\n", enumToString(E), m_fd));
  KMP_DEBUG_ASSERT2(m_fd > 2, "Invalid filedescriptor");
  ioctl(m_fd, PERF_EVENT_IOC_RESET);
  ioctl(m_fd, PERF_EVENT_IOC_ENABLE);
}

//////////////////////////////////////////////////////////////////////
/// @brief Disabling and reading varies a bit between the counters ///
//////////////////////////////////////////////////////////////////////
template <EventCodes E> uint64_t AMDRawEvent<E>::stopAndRead() {
  KA_TRACE(5, ("%s::stopAndRead: fd=%d\n", enumToString(E), m_fd));
  KMP_DEBUG_ASSERT2(m_fd > 2, "Invalid filedescriptor");

  ioctl(m_fd, PERF_EVENT_IOC_DISABLE);

  uint64_t counter = 0;
  if (read(m_fd, &counter, sizeof(uint64_t)) == -1) {
    KA_TRACE(1, ("%s:%d: __kmp_stop_counter(ERROR): Reading counter for "
                 "CPU#%d. Read fail\n",
                 __FILE_NAME__, __LINE__, m_cpu));
    perror("Reason: ");
    close(m_fd);
    return 0;
  }
  if constexpr (E == EventCodes::TOT_DISPATCH_SLOTS) {
    m_accumCounter += counter * 6;
    return counter * 6;
  }
  m_accumCounter += counter;
  return counter;
}

template <EventCodes E> void AMDRawEvent<E>::disableCounter() {
  KMP_DEBUG_ASSERT2(m_fd > 2, "Invalid file descriptor");
  ioctl(m_fd, PERF_EVENT_IOC_DISABLE);
  close(m_fd);
  m_fd = -1;
}

template <EventCodes E> void AMDRawEvent<E>::resetAccumulatedCounter() {
  m_accumCounter = 0;
}

template <EventCodes E> uint64_t AMDRawEvent<E>::accumulatedCounter() {
  return m_accumCounter;
}

///////////////////////////////////////////////////////////////////
/// @brief RawAMDPerfContainer wraps all raw counters           ///
///////////////////////////////////////////////////////////////////
RawAMDPerfContainer::RawAMDPerfContainer(int32_t cpu_id, int32_t gtid)
    : m_cpu(cpu_id), m_gtid(gtid), m_totDisp(cpu_id), m_backendBound(cpu_id),
      m_backendMemNumerator(cpu_id), m_backendMemDenominator(cpu_id),
      m_retiring(cpu_id), m_l1FillDifferentNuma(cpu_id),
      m_l1FillSameCXX(cpu_id), m_l1FillAnotherCXX(cpu_id), m_l1FillAll(cpu_id),
      m_l3Miss(cpu_id) {}

RawAMDPerfContainer &
RawAMDPerfContainer::operator=(RawAMDPerfContainer &&other) {
  m_cpu = other.m_cpu;
  m_gtid = other.m_gtid;
  m_totDisp = other.m_totDisp;
  m_backendBound = other.m_backendBound;
  m_backendMemNumerator = other.m_backendMemNumerator;
  m_backendMemDenominator = other.m_backendMemDenominator;
  m_retiring = other.m_retiring;
  m_l1FillDifferentNuma = other.m_l1FillDifferentNuma;
  m_l1FillSameCXX = other.m_l1FillSameCXX;
  m_l1FillAnotherCXX = other.m_l1FillAnotherCXX;
  m_l1FillAll = other.m_l1FillAll;
  m_l3Miss = other.m_l3Miss;
  KA_TRACE(5, ("RawContainer assign for CPU#%d\n", m_cpu));
  return *this;
}

void RawAMDPerfContainer::initAll() {
  KA_TRACE(5,
           ("RawAMDPerfContainer::initAll: Init AMD counters for %d\n", m_cpu));
  m_totDisp.initCounter();
  m_backendBound.initCounter();
  m_backendMemNumerator.initCounter();
  m_backendMemDenominator.initCounter();
  m_retiring.initCounter();
  m_l1FillDifferentNuma.initCounter();
  m_l1FillSameCXX.initCounter();
  m_l1FillAnotherCXX.initCounter();
  m_l1FillAll.initCounter();
  m_l3Miss.initCounter();
  // TODO: Add the rest
}

void RawAMDPerfContainer::startAll() const {
  KA_TRACE(
      5,
      ("RawAMDPerfContainer::startAll: Starting all AMD counters for CPU#%d\n",
       m_cpu));
  m_totDisp.startCounter();
  m_backendBound.startCounter();
  m_backendMemNumerator.startCounter();
  m_backendMemDenominator.startCounter();
  m_retiring.startCounter();
  m_l1FillDifferentNuma.startCounter();
  m_l1FillSameCXX.startCounter();
  m_l1FillAnotherCXX.startCounter();
  m_l1FillAll.startCounter();
  m_l3Miss.startCounter();
  // TODO: Add the rest
}

AMDRawResults RawAMDPerfContainer::stopAndReadAll() {
  KA_TRACE(5, ("RawAMDPerfContainer::stopAndReadAll: Stop and read AMD "
               "counters for CPU#%d\n",
               m_cpu));
  const auto totDisp = m_totDisp.stopAndRead();

  const auto backendBound = m_backendBound.stopAndRead();
  const auto backendMemNumer = m_backendMemNumerator.stopAndRead();
  const auto backendMemDenom = m_backendMemDenominator.stopAndRead();
  const auto retiring = m_retiring.stopAndRead();
  const auto l1DiffNuma = m_l1FillDifferentNuma.stopAndRead();
  const auto l1SameCXX = m_l1FillSameCXX.stopAndRead();
  const auto l1AnotherCXX = m_l1FillAnotherCXX.stopAndRead();
  const auto l1All = m_l1FillAll.stopAndRead();
  const auto l3Miss = m_l3Miss.stopAndRead();

  const auto backendBoundFrac = frac(backendBound, totDisp);
  const auto backendBoundMemFrac =
      backendBoundFrac * frac(backendMemNumer, backendMemDenom);
  const auto backendBoundCPU =
      backendBoundFrac * (1 - frac(backendMemNumer, backendMemDenom));
  const auto retiringFrac = frac(retiring, totDisp);

  // TODO: Add the rest
  return AMDRawResults(totDisp, l1All, l1DiffNuma, l1SameCXX, l1AnotherCXX,
                       l3Miss, backendBoundFrac, backendBoundMemFrac,
                       backendBoundCPU, retiringFrac);
}

AMDRawResults RawAMDPerfContainer::summarizeCounters() {
  const auto totDisp = m_totDisp.accumulatedCounter();
  const auto backendBound = m_backendBound.accumulatedCounter();
  const auto backendMemNumer = m_backendMemNumerator.accumulatedCounter();
  const auto backendMemDenom = m_backendMemDenominator.accumulatedCounter();
  const auto retiring = m_retiring.accumulatedCounter();
  const auto l1DiffNuma = m_l1FillDifferentNuma.accumulatedCounter();
  const auto l1SameCXX = m_l1FillSameCXX.accumulatedCounter();
  const auto l1AnotherCXX = m_l1FillAnotherCXX.accumulatedCounter();
  const auto l1All = m_l1FillAll.accumulatedCounter();
  const auto l3Miss = m_l3Miss.accumulatedCounter();

  const auto backendBoundFrac = frac(backendBound, totDisp);
  const auto backendBoundMemFrac =
      backendBoundFrac * frac(backendMemNumer, backendMemDenom);
  const auto backendBoundCPU =
      backendBoundFrac * (1 - frac(backendMemNumer, backendMemDenom));
  const auto retiringFrac = frac(retiring, totDisp);

  m_totDisp.resetAccumulatedCounter();
  m_backendBound.resetAccumulatedCounter();
  m_backendMemNumerator.resetAccumulatedCounter();
  m_backendMemDenominator.resetAccumulatedCounter();
  m_retiring.resetAccumulatedCounter();
  m_l1FillDifferentNuma.resetAccumulatedCounter();
  m_l1FillSameCXX.resetAccumulatedCounter();
  m_l1FillAnotherCXX.resetAccumulatedCounter();
  m_l1FillAll.resetAccumulatedCounter();
  m_l3Miss.resetAccumulatedCounter();

  return AMDRawResults(totDisp, l1All, l1DiffNuma, l1SameCXX, l1AnotherCXX,
                       l3Miss, backendBoundFrac, backendBoundMemFrac,
                       backendBoundCPU, retiringFrac);
}

void RawAMDPerfContainer::disableAll() {
  KA_TRACE(
      5,
      ("RawAMDPerfContainer::disableAll: Disabling AMD counters for CPU#%d\n",
       m_cpu));
  m_totDisp.disableCounter();
  m_backendBound.disableCounter();
  m_backendMemNumerator.disableCounter();
  m_backendMemDenominator.disableCounter();
  m_retiring.disableCounter();
  m_l1FillDifferentNuma.disableCounter();
  m_l1FillSameCXX.disableCounter();
  m_l1FillAnotherCXX.disableCounter();
  m_l1FillAll.disableCounter();
  m_l3Miss.disableCounter();
  // TODO: Add the rest
}