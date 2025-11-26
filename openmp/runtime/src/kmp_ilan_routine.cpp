#include "kmp_ilan_routine.h"
#include "kmp.h"
#include "kmp_config.h"
#include "kmp_debug.h"
#include "kmp_os.h"
#include "kmp_ilan_schedule.h"
#include "kmp_ilan_topo.h"
#include <algorithm>
#include <bitset>
#include <cfloat>
#include <climits>
#include <limits>

namespace {
routine_config UNDEFINED_CONFIG = {-1, -1, 0, StealPolicy::NUMA};

kmp_int64 getDiff(kmp_int64 a, kmp_int64 b) { return a > b ? a - b : b - a; }

kmp_int64 getMin(kmp_int64 a, kmp_int64 b) { return a < b ? a : b; }

void printStatsArray(routine_stats_nodes arr) {
  auto i = 0;
  for (const auto &stat : arr) {
    KC_TRACE(1, ("Node[%d] ExecT:%f \n", i, stat.execution_time));
    i++;
  }
}

} // namespace

///
/// @brief This constructor creates a routine object storing information
/// about a taskloop with ID routine_id (memory address of taskloop i.e.
/// routine_entry).
/// @note The initial config is created based on the number of threads
Routine::Routine(kmp_int64 routine_id, kmp_uint32 nthreads)
    : m_routine_id(routine_id),
      inital_nthreads(nthreads),
      m_current_config(getInitialConfig(inital_nthreads)),
      m_1stfastest(UNDEFINED_CONFIG), 
      m_2ndfastest(UNDEFINED_CONFIG),
      m_search_finished(false), 
      m_iteration_count(0),
      MOLDABILITY_GRANULARITY(ILAN::__kmp_ilan_topology().get_numa_size()) {
  KC_TRACE(1, ("Routine::Routine(): \nCreated routine %p with initial config:{"
               "\n\tnthreads=%d,"
               "\n\tntasks=%d,"
               "\n\tmask=%s,"
               "\n\tsteal_policy=%s"
               "\n\tmodability granularity=%d\n}\n"
               ,
               m_routine_id,
               m_current_config.num_threads,
               m_current_config.num_tasks,
               std::bitset<16>(m_current_config.node_mask).to_string().c_str(),
               std::bitset<16>(
                  static_cast<kmp_uint16>(m_current_config.steal_policy)
               ).to_string().c_str(),
               MOLDABILITY_GRANULARITY));
}


///
/// @brief Sets up and returns the initial config. This config
/// is used for the first iteration of the taskloop.
/// nthreads: size of the team
/// initial config:
///    num_threads: nthreads
///    num_tasks: default OpenMP heuristic (nthreads * 10)
///    node_mask: all available NUMA nodes
///    steal_policy: NUMA
///

routine_config Routine::getInitialConfig(kmp_uint32 nthreads) {
  routine_config config;
  config.num_threads = nthreads;
  // @todo: default is same as OpenMP heuristic
  config.num_tasks = nthreads * 10;
  // Use all available NUMA nodes initially

  config.node_mask =
      static_cast<kmp_uint16>(ILAN::__kmp_ilan_topology().get_numa_mask());
  
  if(nthreads < ILAN::__kmp_ilan_topology().get_num_pus()){
    // in case of undersubscription, we need to adjust the node mask
    // e.g. 64 cores, but only 16 threads

    // now just use first N NUMA nodes to cover the threads
    // but actually, we need the team information here to 
    // select the numa nodes based on the team thread affinity
    
    // get the capacity of each NUMA node
    const auto NUMA_SIZE = ILAN::__kmp_ilan_topology().get_numa_size();
    kmp_uint16 required_numa_nodes = 
        static_cast<kmp_uint16>(
          (nthreads + NUMA_SIZE - 1) / NUMA_SIZE); // ceiling division
    
    // select the first required_numa_nodes from the full mask
    kmp_uint16 new_mask = 0;
    kmp_uint16 count = 0;
    for(kmp_uint16 i = 0; i < ILAN::__kmp_ilan_topology().get_num_numa(); i++){
      if( (config.node_mask & (1U << i)) != 0 ){
        new_mask |= (1U << i);
        count++;
        if(count >= required_numa_nodes){
          break;
        }
      }
    }
    KC_TRACE(1, ("Routine::getInitialConfig(): Undersubscription case, "
                 "adjusted node mask from 0x%x to 0x%x for %d threads.\n",
                 config.node_mask, new_mask, nthreads));
                 
    config.node_mask = new_mask;
  }

  // Initial stealing policy: NUMA, only steal within NUMA nodes
  config.steal_policy = StealPolicy::NUMA;
  return config;
}

///
/// @brief Check if binary search is possible.
///
void Routine::initBinarySearch() {
  // just check if there is more than 1 numa node
  // if yes, try half the number of threads
  if (m_current_config.num_threads >= MOLDABILITY_GRANULARITY * 2) {

    m_current_config.num_threads = m_current_config.num_threads / 2;
    KC_TRACE(
        1, ("Routine::initBinarySearch(): Only one previous config."
            " Try half the number of threads (%d threads/2) for routine %p .\n",
            m_current_config.num_threads, m_routine_id));
    return;
  }

  KC_TRACE(2, ("Routine::initBinarySearch(): Binary search not possible: To "
               "few NUMA nodes\n"));
  m_search_finished = true;
  m_1stfastest = m_current_config;
}

///
/// @brief This function selects the number of threads used in the
/// next config, by using binary search.
///
void Routine::binarySearch() {

  kmp_real64 fastest1st_time = calcSlowestNUMAExec(m_1stfastest);
  kmp_real64 fastest2nd_time = calcSlowestNUMAExec(m_2ndfastest);

  KC_TRACE(3, ("\nRoutine::binarySearch():"
               " Comparing old configs for routine %p. \n"
               "Fastest config={%d, %d, %d} execT=%f, "
               " Second fastest={%d, %d, %d} execT=%f.\n",
               m_routine_id, m_1stfastest.num_threads, m_1stfastest.num_tasks,
               static_cast<int>(m_1stfastest.steal_policy), fastest1st_time,
               m_2ndfastest.num_threads, m_2ndfastest.num_tasks,
               static_cast<int>(m_2ndfastest.steal_policy), fastest2nd_time));

  const kmp_int64 diff_threads =
      getDiff(m_1stfastest.num_threads, m_2ndfastest.num_threads);

  const kmp_int64 next_num_threads =
      // / Base: Start from the smaller of the two fastest configs
      getMin(m_1stfastest.num_threads, m_2ndfastest.num_threads) +
      // Offset: Add half the difference, rounded down to NUMA granularity
      (
        (
          // Step 1: Calculate half the difference between the two configs
          // Step 2: Round down to nearest multiple of MOLDABILITY_GRANULARITY
          (diff_threads / 2) / MOLDABILITY_GRANULARITY
          // Step 3: then multiply back to get aligned value
        ) * MOLDABILITY_GRANULARITY
      );

  // Check if the smallest config is fastest.
  // In this case, select the smallest number of threads
  if (m_iteration_count == 3 &&
      m_1stfastest.num_threads < m_2ndfastest.num_threads) {
    if (m_current_config.num_threads == MOLDABILITY_GRANULARITY) {
      m_search_finished = true;
    }

    m_current_config.num_threads = MOLDABILITY_GRANULARITY;
  }

  // Check if a local minima has been found.
  // In this case, select the fastest config.
  else if (diff_threads <= MOLDABILITY_GRANULARITY) {

    m_search_finished = true;
    m_current_config = m_1stfastest;

    KC_TRACE(
        3,
        ("Routine::binarySearch(): Search finished. Select fastest config.\n"));
  }

  // Select the config inbetween the fastest and
  // second fastest config.
  else {
    if (m_current_config.num_threads == next_num_threads) {
      m_search_finished = true;
      m_current_config = m_1stfastest;
    } else {
      m_current_config.num_threads = next_num_threads;
    }

    KC_TRACE(3, ("Routine::binarySearch(): Selecting new config"
                 " based on thread diff: %d, new number of threads: %d.\n",
                 diff_threads, m_current_config.num_threads));
  }
}

///
/// @brief This function select the next config used for executing a taskloop.
/// This includes the moldability algorithm, load balance calculations and
/// setting the node_mask.
///
/// For a certain taskloop, the next config will be selected accordingly:
/// 1. First & second iteration, execute on maxium number of nodes.
/// 2. Third iteration, check if the execution time is below the threshold.
///    In this case, execute on only one node. Otherwise, use binary search to
///    find optimal number of threads.
/// 3. The binary search will continue until the fastest config is found. This
///    config will be used for the rest of the execution.
/// 4. Check if load balancing is required for the fastest config, to
///    determine if the stealing policy needs to be changed.
const routine_config &Routine::getNextConfig() {
  m_iteration_count++;
  bool current_search_state = m_search_finished;

// becareful: NOT DEFINE MOLDABILITY
#ifndef MOLDABILITY
  #ifdef LOADBALANCE
    m_current_config.steal_policy = StealPolicy::FULL;
  #endif
  return m_current_config;
#endif

  /* old version code, hard to read */
  // if (m_current_config.num_threads == 1) {
  //   m_search_finished = true;
  // } else if (m_iteration_count == 1) {
  //   // Do nothing, just keep executing the current config
  // } else if (m_search_finished) {
  //   // Keep running the fastest config
  //   m_current_config = m_1stfastest;
  // } else if (m_iteration_count == 2) {
  //   // init binary search (will update m_current_config)
  //   initBinarySearch();
  // } else {
  //   // If two or more previous configs, try a config inbetween the two fastest
  //   // configs
  //   KC_TRACE(1, ("Routine::getNextConfig(): binarySearch\n"));
  //   binarySearch(); // will update m_current_config
  // }

  if (m_current_config.num_threads == 1) {
    m_search_finished = true;
    // go directly to using current config
  } else if (m_search_finished) {
    // more than 1 thread, and bin search finished
    // Keep running the fastest config
    m_current_config = m_1stfastest;
  } else {
    switch (m_iteration_count) {
      case 1:{
          // the first time, using the maximum number of threads
          // Do nothing, just keep executing the current config
      } break;
      
      case 2:{
        // init binary search (will update m_current_config)
        initBinarySearch();
      } break;

      default: {
        // try a config inbetween the two fastest configs 
        KC_TRACE(2, ("Routine::getNextConfig(): binarySearch\n"));
        binarySearch(); // will update m_current_config
      } break;
    }
  }

  // For now, always set numer of task according to default OpenMP heuristic
  // @hongguang: will be used as grainsize, so the task_num = total/grainsize?
  m_current_config.num_tasks = m_current_config.num_threads * 10;

  // Determine NUMA node placement
  KC_TRACE(2, ("Routine::getNextConfig(): Getting NUMA mask for routine %p\n",
               m_routine_id));
  m_current_config.node_mask = getNUMAMask();

  // Check if load balancing is required.
  #ifdef LOADBALANCE
    if (m_search_finished && !current_search_state) {
      m_current_config.steal_policy = checkLoadBalance();
    }
  #endif

  KC_TRACE(2, ("Routine::getNextConfig(): Routine %p was given config: "
               "{nthreads=%d, ntasks=%d, mask=%s, steal_policy=%d} .\n",
               m_routine_id, m_current_config.num_threads,
               m_current_config.num_tasks,
               std::bitset<16>(m_current_config.node_mask).to_string().c_str(),
               m_current_config.steal_policy));

  // Update current config
  return m_current_config;
}

// This method stores the latest taskloop execution
//
// NOTE: The method relies on the fact that the config used
// for the execution is stored in the current_config variable
// why first execution discarded? because the first execution is
// often with cold cache, and not representative
void Routine::storeExecution(routine_stats_nodes stats) {
  if (m_iteration_count == 0) {
    KC_TRACE(1, ("Routine::storeExecution(): m_iteraion: %d \n\t First execution discarded "
                 "(routine %p)\n",
                 m_iteration_count,
                 m_routine_id));
    return;
  }

  // kmp_uint16 mask = m_current_config.node_mask;

  // Make sure only active NUMA nodes have reported stats

  // auto i = 0;
  // for (const auto &stat : stats) {
  //   if (((1U << i) & mask) == 0 && (stat.execution_time != 0)) {
  //     auto tmp = std::bitset<16>(m_current_config.node_mask);
  //     KC_TRACE(1, ("Routine::storeExecution(): Node mask = 0b%s. Stat non zero "
  //                  "for node [%d] "
  //                  "(routine %p)\n",
  //                  tmp.to_string().c_str(), i, m_routine_id));
  //     KMP_DEBUG_ASSERT(false);
  //   }
  //   i++;
  // }

  // If config doesnt exists, just add the config and stats
  if (m_execution_history.find(m_current_config) == m_execution_history.end()) {
    m_execution_history.emplace(m_current_config, stats);

    KC_TRACE(1, ("Routine:storeExecution[new]: m_iteraion: %d \n\t routine %p inserted new config={%d, "
                 "%d, %d}\n",
                 m_iteration_count,
                 m_routine_id, 
                 m_current_config.num_threads,
                 m_current_config.num_tasks,
                 static_cast<int>(m_current_config.steal_policy)));

  } else {

    KC_TRACE(1, ("Routine:storeExecution[update]: m_iteraion: %d \n\t routine %p has new stats for "
                 "config={%d, %d, %d}.\n",
                  m_iteration_count,
                 m_routine_id, m_current_config.num_threads,
                 m_current_config.num_tasks,
                 static_cast<int>(m_current_config.steal_policy)));
    // For now, we just overwrite the stats with latest run
    m_execution_history.at(m_current_config) = stats;
    KMP_DEBUG_ASSERT(m_search_finished);
  }
  printStatsArray(stats);

  kmp_real64 new_time = calcSlowestNUMAExec(m_current_config);
  kmp_real64 fastest = std::numeric_limits<kmp_real64>::max();
  kmp_real64 second_fastest = std::numeric_limits<kmp_real64>::max();

  // if m_1stfastest or m_2ndfastest is uninitialized
  if (m_1stfastest.num_threads != -1) {
    fastest = calcSlowestNUMAExec(m_1stfastest);
  }

  if (m_2ndfastest.num_threads != -1){
    second_fastest = calcSlowestNUMAExec(m_2ndfastest);
  }

  if (new_time < fastest) {
    KMP_DEBUG_ASSERT(!(m_current_config == m_1stfastest));
    KC_TRACE(1, ("Routine::storeExecution[fast]: "
                "Updating fastest configs\n\t"
                "current fast: %.6f new: %.6f\n", 
                (fastest==std::numeric_limits<kmp_real64>::max()?-1.0:fastest), new_time));
    m_2ndfastest = m_1stfastest;
    m_1stfastest = m_current_config;
  } else if (new_time < second_fastest) {
    KMP_DEBUG_ASSERT(!(m_current_config == m_2ndfastest));
    KC_TRACE(1, ("Routine::storeExecution[2nd]: "
                 "Updating 2nd fastest config\n\t"
                 "current second: %.6f new: %.6f\n", 
                 (second_fastest==std::numeric_limits<kmp_real64>::max()?-1.0:second_fastest), new_time));
    m_2ndfastest = m_current_config;
  }
}

///
/// @brief Calculates the slowest execution time among all NUMA nodes for a
/// certain config
///
kmp_real64 Routine::calcSlowestNUMAExec(const routine_config &config) {

  kmp_real64 slowest = 0;
  const auto &stats = m_execution_history.at(config);
  for (const auto &stat : stats) {
    slowest = std::max(slowest, stat.execution_time);
  }

  KMP_DEBUG_ASSERT(slowest > 0);
  return slowest;
}

///
/// @brief This function will return the NUMA node mask for the current config.
/// @note This function assumes that m_current_config does not
/// change before next taskloop execution.
/// cases:  
/// 1. threads == 1: just use the current node mask
/// 2. threads > cores number: oversubscription
///    eg. 1 core, but 2 threads,
/// 3. threads < cores number: in some tests, 
///    eg. gromacs test, 64 cores but with 2 threads,
/// 4. threads == cores number: normal case
///
kmp_uint16 Routine::getNUMAMask() const {
  /* 
    any case of below will just use the initial node mask,
    the full node mask assigned to the omp runtime
   */
  if (
      // first iteration, use all nodes
      m_iteration_count == 1 ||  
      // only one thread, use current mask
      m_current_config.num_threads == 1 || 
      // oversubscription, use all nodes
      m_current_config.num_threads >= ILAN::__kmp_ilan_topology().get_num_pus() 
    ) {
    return m_current_config.node_mask;
  }

  /*
    well now, we are in case: threads <= PUs number
    so we need to decide which NUMA nodes to use
   */

  const auto NODE_PER_SOCKET =
      ILAN::__kmp_ilan_topology().get_num_numa() / ILAN::__kmp_ilan_topology().get_num_socket();
  
  
  const auto NUMA_SIZE = ILAN::__kmp_ilan_topology().get_numa_size();
  
  // todo, when just use part of the cores in runtime? (done)
  // const auto ncores = ILAN::__kmp_ilan_topology().get_num_pus();
  
  // Step 1: Find the execution history for full-thread configuration (the initial one)
  // This represents the baseline performance across all NUMA nodes
  auto elem = std::find_if(
      m_execution_history.begin(), m_execution_history.end(),
      [this](const auto &kv) 
        { return kv.first.num_threads == this->inital_nthreads; });
  
  // char buf[128];
  // snprintf(buf, sizeof(buf), "threads=%d, cores=%d", ncores, ncores);
  KMP_DEBUG_ASSERT(elem != m_execution_history.end());

  // Step 2: Find the fastest NUMA node from historical stats
  // This node showed the best performance and will be our starting point
  const auto &stats = elem->second;
  auto min_iter =
      std::min_element(stats.begin(), stats.end(),
                       [](const routine_stats &lhs, const routine_stats &rhs) {
                         return lhs.execution_time < rhs.execution_time;
                       });

  // Get the index of the fastest node
  // not the id of the node, but the index in stats array,
  // also the bit index in node_mask
  auto fastest_index = std::distance(stats.begin(), min_iter);
  KC_TRACE(2, ("Routine::getNUMAMask(): Fastest index = %d\n", fastest_index));

  auto fastest_numa_id = ILAN::__kmp_ilan_topology().get_numa_id_from_index(fastest_index);
  KC_TRACE(2, ("Routine::getNUMAMask(): Fastest NUMA ID = %d\n", fastest_numa_id));

  // Step 3: Calculate how many NUMA nodes we need
  // Example: If we need 24 threads and NUMA_SIZE = 8
  //   -> We need 3 NUMA nodes total
  //   -> numaCount = (24 / 8) - 1 = 2 additional nodes (we already 
  // have 1, the fastest one)
  //  
  
  // how many numa nodes with the current number of threads needed
  // making sure at least one numa node is selected
  auto numaCount = (m_current_config.num_threads + NUMA_SIZE - 1) / NUMA_SIZE;

  // we already have one fastest node
  numaCount = numaCount - 1;
          
  KC_TRACE(2, ("Routine::getNUMAMask(): Need total %d NUMA nodes, "
               "selecting %d additional nodes.\n",
               numaCount + 1, numaCount));
  KMP_DEBUG_ASSERT(numaCount >= 0);
  KMP_DEBUG_ASSERT(numaCount < ILAN::__kmp_ilan_topology().get_num_numa());

  // Step 4: Start with the fastest NUMA node
  // kmp_uint16 mask = static_cast<kmp_uint16>(1U << index);
  kmp_uint16 mask = static_cast<kmp_uint16>(1U << fastest_numa_id);

  // Step 5: Find additional NUMA nodes
  // Start from the beginning of the same socket as the fastest node
  auto socket_start_index = (fastest_index / NODE_PER_SOCKET) * NODE_PER_SOCKET;
  auto current_index = socket_start_index;
  

  while (numaCount > 0) {
    // Convert current index to NUMA ID
    auto current_numa_id = ILAN::__kmp_ilan_topology().get_numa_id_from_index(current_index);
    
    // Check if this NUMA node is not already in the mask
    if (((1U << current_numa_id) & mask) == 0) {
      mask |= 1U << current_numa_id;
      numaCount--;
      KC_TRACE(2, ("Routine::getNUMAMask(): Added NUMA ID %d (index %d) to mask\n", 
                   current_numa_id, current_index));
    }
    
    // Move to next index, wrap around within available NUMA nodes
    current_index = (current_index + 1) % ILAN::__kmp_ilan_topology().get_num_numa();
  }

  KC_TRACE(2, ("Routine::getNUMAMask(): Final mask = 0x%x\n", mask));
  return mask;
}

///
/// @brief Decides whether load balancing is required based
/// on ratio between fastest and slowest NUMA node.
///
StealPolicy Routine::checkLoadBalance() {
  StealPolicy policy = StealPolicy::NUMA;

  kmp_real64 slowest = 0.0;
  kmp_real64 fastest = DBL_MAX;

  for (const auto &stat : m_execution_history.at(m_current_config)) {
    kmp_real64 exec_time = stat.execution_time;
    // Find slowest
    if (exec_time > slowest) {
      slowest = exec_time;
    }
    // Find fastest
    if (exec_time < fastest && exec_time != 0) {
      fastest = exec_time;
    }
  }

  kmp_real64 diff = slowest / fastest;

  if (diff >= LOAD_BALANCE_REQUIRED_FACTOR) {
    policy = StealPolicy::FULL;
  }

  KC_TRACE(2, ("Routine::checkLoadbalance(): Policy %d selected for routine "
               "%p. Fastest:%f, "
               "Slowest:%f, diff:%f\n",
               policy, m_routine_id, fastest, slowest, diff))

  return policy;
}
