#pragma once

#include "kmp_ilan_perf.h"
#include "kmp_os.h"

#include <unordered_map>
#include <cfloat>
#include <climits>
#include <vector>

union kmp_info;
constexpr kmp_real64 LOAD_BALANCE_REQUIRED_FACTOR = 1.0;

/*
so why use int16 to operate the td_affin_mask ?
why FULL is 1U << 15 instead of 0xFFFF ?

Because of this:

  1.thread->th.steal_mask = (1U << numaId) | static_cast<kmp_uint16>(StealPolicy::FULL);

  2.if ((__kmp_threads[gtid]->th.steal_mask & taskdata->td_affin_mask) == 0) return NULL;

the initial td_affin_mask == the id of the numa node the task is assigned to.
when using NUMA steal policy:
  td_affin_mask |= StealPolicy::NUMA  => no change

when using FULL steal policy:
  td_affin_mask |= StealPolicy::FULL => set the highest bit to 1
  so that & operation with any steal_mask will be non-zero,

*/

enum class StealPolicy : kmp_uint16 {
  NUMA = 0,                         // 0000 0000 0000 0000
  SOCKET = 1,                       // 0000 0000 0000 0001 (never used)
  FULL = (1U << 15),                // 1000 0000 0000 0000
  TASK_GENERATION = (1U << 16) - 1  // 1111 1111 1111 1111
};

struct routine_config {
  // number of threads used to execute the routine
  kmp_int64 num_threads;
  // 
  kmp_int64 num_tasks;
  // bitmask of NUMA nodes used
  kmp_uint16 node_mask;

  StealPolicy steal_policy;

  bool operator==(const routine_config &other) const {
    return (num_threads == other.num_threads) &&
           (num_tasks == other.num_tasks) &&
           (node_mask == other.node_mask) &&
           (steal_policy == other.steal_policy);
  }

};



struct routine_config_hash {
  std::size_t operator()(const routine_config &rc) const {
    std::size_t h1 = std::hash<kmp_int64>{}(rc.num_threads);
    std::size_t h2 = std::hash<kmp_int64>{}(rc.num_tasks);
    std::size_t h3 = std::hash<kmp_uint16>{}(rc.node_mask);
    std::size_t h4 = std::hash<int>{}(static_cast<int>(rc.steal_policy));
    return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
  }
};

class Routine {

private:
  // the address of the routine function
  kmp_int64 m_routine_id;

  /*basic information of the rountine*/ 
  // number of threads at the beginning
  // set by the team threads size
  // for handling subscription or oversubscription
  // cases. In these cases, the number of threads
  // does not equal to the number of PUs.
  kmp_int32 inital_nthreads;   

  // history of all executed configurations,
  // used for searching the best configuration
  std::unordered_map<routine_config, routine_stats_nodes, routine_config_hash>
      m_execution_history;
  
  // current configuration being tested 
  routine_config m_current_config;

  // best configuration found so far
  routine_config m_1stfastest;
  // second best configuration found so far
  routine_config m_2ndfastest;

  // inner state for binary search
  // searching algrithm: see binarySearch()
  bool m_search_finished;
  // count of how many times getNextConfig() has been called
  kmp_uint32 m_iteration_count;

  // granularity for moldability, used only in binary search
  // the number of threads is changed by this granularity
  kmp_uint32 MOLDABILITY_GRANULARITY;

  // create a new configuration and set it according to nthreads
  // it will be used as the initial configuration for searching
  /// @brief getInitialConfig
  /// @param nthreads number of threads in the team
  /// @return routine_config
  routine_config getInitialConfig(kmp_uint32 nthreads);
  
  void initBinarySearch();
  void binarySearch();

  kmp_real64 calcSlowestNUMAExec(const routine_config &config);
  kmp_uint16 getNUMAMask() const;
  StealPolicy checkLoadBalance();

  inline bool isXFasterThanY(const routine_config &X, const routine_config &Y) {
    return calcSlowestNUMAExec(X) < calcSlowestNUMAExec(Y);
  }

public:
  //  routine_id (memory address of taskloop i.e. routine_entry).
  explicit Routine(kmp_int64 routine_id, kmp_uint32 nthreads);
  // used by __kmp_select_config and 
  const routine_config &getCurrentConfig() const { return m_current_config; }

  // used only by __kmp_select_config when re-/starting the taskloop scheduling
  // returns the next configuration to be tested
  // based on the previous execution results
  const routine_config &getNextConfig();
  void storeExecution(routine_stats_nodes stats);
};
