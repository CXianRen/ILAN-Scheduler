#include "kmp_ilan_perf.h"
#include "kmp.h"
#include "kmp_debug.h"
#include "kmp_os.h"
#include "kmp_ilan_perf_objects.h"
#include "kmp_ilan_topo.h"

#include <asm/unistd_64.h>
#include <cstdint>
#include <sched.h>
#include <linux/perf_event.h>
#include <unistd.h>
#include <sys/ioctl.h>

namespace {
  inline constexpr int perf_id(PerfEvents event) {
    return static_cast<int>(event);
  }

  inline kmp_real64 frac(uint64_t numerator, uint64_t denominator) {
    return static_cast<kmp_real64>(numerator) /
          static_cast<kmp_real64>(denominator);
  }

  template <PerfEvents ev> inline constexpr int perf_event() {
    if constexpr (ev == PerfEvents::TOT_CYCLES)
      return PERF_COUNT_HW_CPU_CYCLES;
    if constexpr (ev == PerfEvents::TOT_INSTRUCTIONS)
      return PERF_COUNT_HW_INSTRUCTIONS;
  }

  const char *enumToString(PerfEvents event) {
    switch (event) {
    case PerfEvents::TOT_CYCLES:       // Total CPU Cycles
      return "TOT_CYCLES";
    case PerfEvents::TOT_INSTRUCTIONS: // Total Instructions
      return "TOT_INSTRUCTIONS";
    default:
      return "UNKNOWN";
    }
  }

  int32_t perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu,
                          int group_fd, unsigned long flags) {
    return static_cast<int32_t>(
        syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
  }


  template <PerfEvents ev>
  void init_perf_event(kmp_info_t *thread, perf_event_attr *pe, int32_t cpu_id) {
    pe->type = PERF_TYPE_HARDWARE;
    pe->config = perf_event<ev>();
    int32_t fd = perf_event_open(pe, 0, cpu_id, -1, 0);
    thread->th.perf_stats[perf_id(ev)] = fd;

    if (fd == -1) {
      KA_TRACE(
          1,
          ("%s:%d: __kmp_init_perf_event(ERROR): #T%d = CPU#%d: Cannot open %s\n",
          __FILE_NAME__, __LINE__, __kmp_gtid_from_thread(thread), cpu_id,
          enumToString(ev)));
      perror("Reason: ");
      return;
    }

    KA_TRACE(5, ("%s:%d: __kmp_init_perf_event: (E#%d, FD#%d, T#%d, CPU#%d).\n",
                __FILE_NAME__, __LINE__, ev, fd, __kmp_gtid_from_thread(thread),
                cpu_id));
  }

  template <PerfEvents ev> void enable_perf_event(kmp_info_t *thread) {
    int32_t cpu_id = sched_getcpu();
    kmp_int64 gtid = __kmp_gtid_from_thread(thread);

    int32_t fd = thread->th.perf_stats[perf_id(ev)];

    KMP_DEBUG_ASSERT(fd > 2);

    ioctl(fd, PERF_EVENT_IOC_RESET);

    uint64_t counter = 0;
    if (read(fd, &counter, sizeof(uint64_t)) == -1) {
      KA_TRACE(1, ("%s:%d: __kmp_enable_perf_event(ERROR): Reading counter for "
                  "T#%d. Read fail\n",
                  __FILE_NAME__, __LINE__, gtid));
      perror("Reason: ");
      close(fd);
      return;
    }

    KA_TRACE(5, ("%s:%d: __kmp_enable_perf_event: (E#%d, FD#%d, T#%d, CPU#%d)\n",
                __FILE_NAME__, __LINE__, ev, fd, gtid, cpu_id));

    ioctl(fd, PERF_EVENT_IOC_ENABLE);
  }

  template <PerfEvents ev>
  uint64_t stop_perf_event(kmp_info_t *thread, int32_t cpu_id) {
    // Stop event and read
    const int fd = thread->th.perf_stats[perf_id(ev)];

    KMP_DEBUG_ASSERT(fd > 2);

    ioctl(fd, PERF_EVENT_IOC_DISABLE);

    uint64_t counter = 0;
    if (read(fd, &counter, sizeof(uint64_t)) == -1) {
      KA_TRACE(1, ("%s:%d: __kmp_stop_perf_event(ERROR): Reading counter for "
                  "CPU#%d. Read fail\n",
                  __FILE_NAME__, __LINE__, cpu_id));
      perror("Reason: ");
      close(fd);
      thread->th.perf_stats[perf_id(ev)] = -1;
      return 0;
    }

    KA_TRACE(
        5, ("%s:%d: __kmp_stop_perf_event: (E#%d, FD#%d, T#%d, CPU#%d, Val=%d)\n",
            __FILE_NAME__, __LINE__, ev, fd, __kmp_gtid_from_thread(thread),
            cpu_id, counter));

    thread->th.perf_accum[perf_id(ev)] += counter;
    return counter;
  }

  template <PerfEvents ev> void disable_perf_event(kmp_info_t *thread) {
    int32_t cpu_id = sched_getcpu();

    // Disable event
    const int fd = thread->th.perf_stats[perf_id(ev)];
    if (fd <= 2) // Counter was unavailable
    {
      return;
    }

    KA_TRACE(5, ("%s:%d: __kmp_disable_perf_event: (E#%d, FD#%d, T#%d, CPU#%d)\n",
                __FILE_NAME__, __LINE__, ev, fd, __kmp_gtid_from_thread(thread),
                cpu_id));

    thread->th.perf_stats[perf_id(ev)] = -1; // reset fd
    ioctl(fd, PERF_EVENT_IOC_DISABLE);
    close(fd);
  }
} // namespace

///
/// @brief This function initialized the perf conters for a specific thread and
/// CPU core, by opening all perf event file descriptors. This function should
/// only be called once per thread.
//  
/*
    Record:
      the total cycle
      the total instruction
*/
void Perf::__kmp_ilan_init_counters(kmp_info_t *thread, int32_t gtid) {
  int32_t cpu_id = sched_getcpu();

#ifdef AMD_PERF
  thread->th.perf_container = RawAMDPerfContainer(cpu_id, gtid);
  thread->th.perf_container.initAll();
#endif

  KA_TRACE(1, ("%s:%d: __kmp_init_counter(entered): T#%d = CPU#%d.\n ",
               __FILE_NAME__, __LINE__, gtid, cpu_id));

  // Init perf event
  perf_event_attr pe;
  memset(&pe, 0, sizeof(perf_event_attr));
  pe.size = sizeof(perf_event_attr);
  pe.disabled = 1;
  pe.exclude_kernel = 1; // exclude kernel level event, only user level event
  pe.inherit = 0;        // disable inherit by children process
  pe.exclude_hv = 1;     // ignore vm layer, same as exclude kernel

  init_perf_event<PerfEvents::TOT_CYCLES>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread, &pe, cpu_id);
}

///
/// @brief This function disables all perf events for a specific thread. This
/// function should only be called once per thread.
///
void Perf::__kmp_ilan_deinit_counters(kmp_info_t *thread) {

  disable_perf_event<PerfEvents::TOT_CYCLES>(thread);
  disable_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread);

#ifdef AMD_PERF
  thread->th.perf_container.disableAll();
#endif
}

///
/// @brief This function enables all perf events for a specific thread.
///
/*
    Rest count and start counting
*/
void Perf::__kmp_ilan_start_counters(kmp_info_t *thread) {
  int32_t gtid = __kmp_gtid_from_thread(thread);
  int32_t cpu_id = sched_getcpu();

  KA_TRACE(3, ("%s:%d: __kmp_start_counter(entered): T#%d = CPU#%d.\n ",
               __FILE_NAME__, __LINE__, gtid, cpu_id));

  // Start perf counters and execution time
  __kmp_read_system_time(&thread->th.time);
  enable_perf_event<PerfEvents::TOT_CYCLES>(thread);
  enable_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread);

#ifdef AMD_PERF
  thread->th.perf_container.startAll();
#endif
}

///
/// @brief This function stops and resets all perf events for a specific
/// thread.
///
/*
  Stop counting and read counter
  read it and then accumulate 
  thread->th.perf_accum[perf_id(ev)] += counter;
*/
void Perf::__kmp_ilan_stop_counters(kmp_info_t *thread, int32_t gtid,
                               kmp_int32 task_id) {
  int32_t cpu_id = sched_getcpu();

  uint64_t tot_cycles = stop_perf_event<PerfEvents::TOT_CYCLES>(thread, cpu_id);
  uint64_t tot_ins =
      stop_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread, cpu_id);

#ifdef AMD_PERF
  AMDRawResults results = thread->th.perf_container.stopAndReadAll();
#endif

  kmp_real64 current_time = 0;
  __kmp_read_system_time(&current_time);
  kmp_real64 elapsed_time = current_time - thread->th.time;

  KA_TRACE(4, ("%s:%d: __kmp_ilan_stop_counters: Counters for Task %p executing "
               "routine %p on CPU#%d (T#%d):\n"
               "      - Tot cycles = %ld\n"
               "      - Tot ins = %ld\n"
#ifdef AMD_PERF
               "  # AMD raw ratios:\n"
               "      - TotDisp = %lu\n"
               "      - L1 Fills All = %lu\n"
               "      - L1 Fills Different NUMA = %lu\n"
               "      - L1 Fills same CXX = %lu\n"
               "      - L1 Fills another CXX = %lu\n"
               "      - L3 Misses = %lu\n"
               "      - Retiring fraction = %lf\n"
               "      - Backend bound = %lf\n"
               "      - Backend bound Memory = %lf\n"
               "      - Backend bound CPU = %lf\n"
#endif
               "      - Execution time = %f\n",
               __FILE_NAME__, __LINE__, task_id, thread->th.routine_id, cpu_id,
               gtid, tot_cycles, tot_ins
#ifdef AMD_PERF
               ,
               results.m_totDisp, results.m_l1All, results.m_l1DiffNuma,
               results.m_l1SameCXX, results.m_l1AnotherCXX, results.m_l3Miss,
               results.m_retiring, results.m_backend, results.m_backendMem,
               results.m_backendCPU
#endif
               ,
               elapsed_time));
}


/////////////////////////////////////////////////////////////////////////////
///                 Always used by Routine class                          ///
/////////////////////////////////////////////////////////////////////////////

///
/// @brief This function summarizes the counter stats for each thread
/// and aggregates them on NUMA node granularity.
///
static void __kmp_ilan_summarize_taskloop_stats(kmp_team *team,
                                    routine_stats_nodes &numaSummary,
                                    const kmp_real64 taskloop_start_time) {
                                      
  // always assuming all nodes are used
  auto num_numa = ILAN::__kmp_ilan_topology().get_num_total_numa();
  KMP_DEBUG_ASSERT(num_numa);

  std::vector<kmp_real64> IPCs(num_numa, 0.0);
  std::vector<kmp_real64> finish_times(num_numa, taskloop_start_time);
  std::vector<kmp_uint32> counts(num_numa, 0);

  // iterate over all threads in the team
  // and collect stats per thread
  kmp_uint32 team_threads_num = static_cast<kmp_uint32>(team->t.t_nproc);
  KMP_DEBUG_ASSERT(team_threads_num>0);
  for (kmp_uint32 i = 0; i < team_threads_num; i++) {

    kmp_info_t *thread = team->t.t_threads[i];
    auto gtid = __kmp_gtid_from_thread(thread);
    auto numa_id = ILAN::__kmp_ilan_topology().get_numa_id(gtid);
    KMP_DEBUG_ASSERT(numa_id>=0);

    // total instructions
    const auto tot_ins =
        thread->th.perf_accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)];
    // total cycles
    const auto tot_cyc = thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)];
    
    // if either is zero, skip
    if (tot_cyc != 0) {
      KA_TRACE(1,
                 ("__kmp_summarize_taskloop_stats: T#%2d (NUMA#%d): Tot ins = "
                  "%ld, Tot cyc = %ld, IPC = %lf\n",
                  gtid, numa_id, tot_ins, tot_cyc, frac(tot_ins, tot_cyc)));
      IPCs[numa_id] += frac(tot_ins, tot_cyc);
    }

    // Reset thread stats
    thread->th.perf_accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)] = 0;
    thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)] = 0;

    // find the max finish time
    if (finish_times[numa_id] < thread->th.task_finish_time) {
      finish_times[numa_id] = thread->th.task_finish_time;
    }
    counts[numa_id] += 1;
  }
  
  // Aggregate per NUMA node
  kmp_real64 ipc = 0.0, exec_time = 0.0;
  for (kmp_uint32 n = 0; n < num_numa; n++) {
    ipc = 0.0;
    exec_time = 0.0;
    if (counts[n] == 0)
    {
      // no thread in this NUMA node
    }
    else
    {
      // average IPC over all threads in the NUMA node
      ipc = IPCs[n] / static_cast<kmp_real64>(counts[n]);
      // the max finish time - taskloop start time
      exec_time = finish_times[n] - taskloop_start_time;

      numaSummary.push_back({ipc, exec_time});
      KA_TRACE(1,
                 ("__kmp_summarize_taskloop_stats: NUMA node %d has %d threads\n",
                  n, counts[n]));
    }

    KA_TRACE(1,
               ("__kmp_summarize_taskloop_stats: NUMA node %d\n"
                "    - IPC: %lf\n"
                "    - Exec time: %lf\n",
                n, 
                ipc, 
                exec_time));
  }
}

///
/// @brief This function returns the perf metrics from
/// the aggregated perf counters.
///
void Perf::__kmp_get_taskloop_stats(kmp_team *team,
                                    routine_stats_nodes &ret_stats,
                                    const kmp_real64 taskloop_start_time) {
                                      
  __kmp_ilan_summarize_taskloop_stats(team, ret_stats, taskloop_start_time);
}
