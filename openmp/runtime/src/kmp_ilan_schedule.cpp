#include "kmp_ilan_schedule.h"

#include "kmp.h"
#include "kmp_debug.h"
#include <bitset>
#include <unordered_map>
#include "kmp_os.h"
#include "kmp_ilan_perf.h"
#include "kmp_ilan_routine.h"
#include "kmp_ilan_topo.h"

#include "kmp_affinity.h"

#include <immintrin.h>

namespace {

constexpr kmp_uint32 NUM_LOAD_STRICT_TASK = 3;

#ifdef MOLDABILITY

inline kmp_uint8 bitCount(kmp_uint64 mask) {
#if __has_builtin(__builtin_popcountll)
  return static_cast<kmp_uint8>(__builtin_popcountll(mask));
#else
  return 0; // Impl needed
#endif
}

inline kmp_uint8 bitScan(kmp_uint64 mask) {
#if __has_builtin(__builtin_ctzll)
  return static_cast<kmp_uint8>(__builtin_ctzll(mask));
#else
  return 0; // Impl needed
#endif
}

inline kmp_uint8 findBit(kmp_uint16 nodeMask, const kmp_uint8 count) {
  for (auto i = count; i > 0; i--) {
    nodeMask &= nodeMask - 1;
  }
  KMP_DEBUG_ASSERT(nodeMask > 0);
  return bitScan(nodeMask);
}

#endif

inline kmp_uint64 min(const kmp_uint64 a, const kmp_uint64 b) {
  if (a < b) {
    return a;
  }
  return b;
}

inline kmp_uint64 max(const kmp_uint64 a, const kmp_uint64 b) {
  if (a > b) {
    return a;
  }
  return b;
}

// Shared globals: used by summarizing thread
// Map containing all routines
std::unordered_map<kmp_int64, Routine> g_routine_map; 

kmp_real64 routine_timer;

} // namespace


///
/// @brief Returns the info about the policy selected for the current routine.
///
Schedule::PolicyInfo Schedule::__kmp_get_policy_info(kmp_info *thread,
                                                     kmp_int64 routine_id) {
  if (thread->th.th_team_nproc == 1) {
    // todo:@hongguang: I don't get it.  // ? omp single ? omp master?
    KD_TRACE(1, ("__kmp_get_policy_info: Single thread, "
                 "return default policy info: (GENERATION, FULL)\n"));
    return PolicyInfo(static_cast<kmp_uint16>(StealPolicy::TASK_GENERATION),
                      // should |= __kmp_ilan_topology->get_numa_mask() ?
                      // but it is ok since thread is limited to N
                      static_cast<kmp_uint16>(StealPolicy::FULL));
  }


#ifdef MOLDABILITY
  KMP_DEBUG_ASSERT(g_routine_map.find(routine_id) != g_routine_map.end());
  const auto &config = g_routine_map.at(routine_id).getCurrentConfig();
  const kmp_uint16 node_mask = config.node_mask;
  // StealPolicy::NUMA = 0, so only steal within node_mask
  // node_mask: bitmask representing allowed NUMA nodes, and will be changed 
  // when moldability is active (binary search)
  const kmp_uint16 available_steal_mask =
      config.steal_policy == StealPolicy::FULL
          ? node_mask
          : static_cast<kmp_uint16>(StealPolicy::NUMA); 
#else
  // ??? why 0 @hongguang 
  const kmp_uint16 node_mask = 0;
  const kmp_uint16 available_steal_mask = static_cast<kmp_uint16>(StealPolicy::FULL);
#endif

 KD_TRACE(1, ("__kmp_get_policy_info: policy info for:"
                "\n\troutine %p,"
                "\n\tteam size=%d,"
                "\n\tnode_mask=0x%x,"
                "\n\tavailable_steal_mask=0b%s."
                "\n",
               routine_id, 
               thread->th.th_team_nproc,
               node_mask, 
               std::bitset<16>(available_steal_mask).to_string().c_str()));

  return PolicyInfo(node_mask, available_steal_mask);
}




///
/// @brief This function returns the base node of the NUMA node
/// containing the processor corresponding to tid.
///
// kmp_int32 Schedule::__kmp_get_numa_base(kmp_int32 tid) {
//   const auto numaCores = ILAN::__kmp_ilan_topology().get_num_pus();
//   const auto numNuma = ILAN::__kmp_ilan_topology().get_num_numa();
//   KMP_DEBUG_ASSERT(numaCores);
//   KMP_DEBUG_ASSERT(numNuma);

//   const auto numaSize = numaCores / numNuma;
//   KMP_DEBUG_ASSERT(numaSize);
//   return static_cast<kmp_int32>((tid / numaSize) * numaSize);
// }

///
/// @brief This function selects the thread_data queue to put the
/// taskdata on based on td_task_place_tid (calculated in
/// __kmp_set_task_affinity (to be called when 
/// task was generating)). It might update the affinity mask of the task to
/// enable load balancing by __kmp_get_load_balance_mask.
///
kmp_thread_data_t *
Schedule::__kmp_select_thread_data_queue(kmp_task_team *task_team,
                                         kmp_taskdata_t *taskdata) {

  const auto nthreads = task_team->tt.tt_nproc;
  KMP_DEBUG_ASSERT(taskdata);

  char buf[256];
  sprintf(buf, "Task_place_tid=%d, nthreads=%d",
          taskdata->td_task_place_tid, nthreads);
  // KMP_DEBUG_ASSERT(taskdata->td_task_place_tid < nthreads);
  KMP_DEBUG_ASSERT2(taskdata->td_task_place_tid < nthreads, buf);

  kmp_thread_data_t *thread_data =
      &task_team->tt.tt_threads_data[taskdata->td_task_place_tid];
  // ilan_numa_master_thread the thread we put the task onto
  kmp_info_t *ilan_numa_master_thread = thread_data->td.td_thr;
  if (taskdata->td_affin_mask !=
      static_cast<kmp_uint16>(StealPolicy::TASK_GENERATION)) {
#ifdef MOLDABILITY
    const kmp_uint16 available_steal = taskdata->td_available_steal;
#else
    const kmp_uint16 available_steal =
        static_cast<kmp_uint16>(StealPolicy::FULL);
#endif
    // __kmp_get_load_balance_mask return StealPolicy::NUMA or available_steal
    // based on how many tasks in the ilan_numa_master_thread's deque so far
    // if larger a threshold, return available_steal else return StealPolicy::NUMA
    taskdata->td_affin_mask |= Schedule::__kmp_get_load_balance_mask(
        ilan_numa_master_thread, thread_data, available_steal);
  }

  KD_TRACE(3, ("%s:%d: __kmp_optimal_thread: Base NUMA thread tid=%d\n ",
               __FILE_NAME__, __LINE__, taskdata->td_task_place_tid));
  return thread_data;
}

///
/// @brief This function sets up the affinity of taskloop tasks.
/// The affinity is based on the iteration range of the tasks and
/// decides which NUMA node this task is put on.
/// @note The affinity mask of the task (td_affin_mask) will be updated
/// in __kmp_select_thread_data_queue() and might be tagged for load balancing.
void Schedule::__kmp_set_task_affinity(kmp_info *thread,
                                       kmp_taskdata_t *taskdata,
                                       const kmp_int64 routine_id,
                                       const PolicyInfo &policyInfo,
                                       const kmp_uint64 lb, const kmp_uint64 ub,
                                       const kmp_uint64 glob_ub) {
  KMP_DEBUG_ASSERT(taskdata);

  kmp_team_t *team = thread->th.th_team;
  const auto nthreads = static_cast<uint32_t>(team->t.t_nproc);

  // Info based on topology
#ifdef MOLDABILITY
  // Load balance mask is the same as node_mask when moldability is active
  const auto numNuma = bitCount(policyInfo.node_mask);
#else
  const auto numNuma = ILAN::__kmp_ilan_topology().get_num_numa();
#endif
  KMP_DEBUG_ASSERT(numNuma);
  const kmp_uint8 numaNodeSize = ILAN::__kmp_ilan_topology().get_numa_size();
  KMP_DEBUG_ASSERT(numaNodeSize);

  // When single thread is executing
  // ? when would this happen ?
  if (nthreads == 1) {
    KD_TRACE(1, ("%s:%d: __kmp_set_task_affinity: Single thread, "
                 "set default affinity for routine %p\n",
                 __FILE_NAME__, __LINE__, routine_id));
    // if you are confused, read the example following this section first.

    // todo: @hongguang: what about oversubscription and non-full subscription here? 
    taskdata->td_task_place_tid = (static_cast<kmp_uint8>(__kmp_tid_from_gtid(
                                       __kmp_gtid_from_thread(thread))) /
                                   numaNodeSize) *
                                  numaNodeSize;
    taskdata->td_affin_mask = static_cast<kmp_uint16>(StealPolicy::FULL);
    taskdata->td_available_steal = static_cast<kmp_uint16>(StealPolicy::FULL);
    return;
  }

/*
  Example:
  Given a machine with 8 NUMA nodes, 8 cores per NUMA node.
  Total iterations: glob_ub = 1000 (loop iterates from 0 to 999)
  Current task range: lb = 100, ub = 200

  So:
    midRange: the middle index of the loop range (lb, ub)
              = (lb + ub) / 2 = 150

    bucketSize: each NUMA node is regarded as a bucket,
                and each bucket can hold bucketSize iterations.
                Here, we compute bucketSize as
                max(glob_ub / numNuma, numaNodeSize)
                glob_ub / numNuma = 1000 / 8 = 125
                numaNodeSize = 100 (assume minimum per node)
                → bucketSize = max(125, 100) = 125

    bucketId: computes which bucket (NUMA node) this task (iteration range)
              should be placed into:
              bucketId = min(midRange / bucketSize, numNuma - 1)
                       = min(150 / 125, 7) = 1

    KMP_DEBUG_ASSERT(bucketId < numNuma);

  Visualization:

  NUMA nodes (buckets):  [0]       [1]       [2]       [3]       [4]       [5]       [6]       [7]
  bucketSize range:      0-124     125-249   250-374   375-499   500-624   625-749   750-874   875-999
  Task iteration range:   100 - 200
  midRange = 150 → falls into bucket #1
*/
  const auto midRange = (lb + ub) / 2;
  const auto bucketSize = max(glob_ub / numNuma, numaNodeSize);
  const auto bucketId =
      static_cast<kmp_uint8>(min((midRange / bucketSize), numNuma - 1));
  KMP_DEBUG_ASSERT(bucketId < numNuma);
#ifdef MOLDABILITY
  const auto numaId = findBit(policyInfo.node_mask, bucketId);
#else
  const auto numaId = bucketId;
#endif

  // numaId is the physical NUMA node id.
  taskdata->td_affin_mask = static_cast<kmp_uint16>(1U << numaId);
  // Each numa node has numaNodeSize thread. 
  // the task is put to the primary thread of that number node.

  // todo: @hongguang: here is the problem, need to map numaId to actual thread id.
  taskdata->td_task_place_numa_id = numaId;
  
  /*
   * @hongguang: need a better way to find the actual thread id
     numaId is global numa node id.
   */
   // find the first thread in the numa node
  // taskdata->td_task_place_tid = numaId * numaNodeSize; 
  taskdata->td_task_place_tid = bucketId * numaNodeSize; 

      
  taskdata->td_available_steal = policyInfo.available_steal_mask;

  KD_TRACE(3, ("%s:%d: __kmp_set_task_affinity: for routine %p: Nthreads=%d, "
               "Nnuma=%d, "
               "numaid=%d,"
               "bucketSize=%lu "
               "MidIter#%d => Affin_mask=0b%s.\n"
               "Task_place=%d\n",
               __FILE_NAME__, __LINE__, routine_id, nthreads, 
               numNuma, 
               numaId,
               bucketSize, 
               midRange, std::bitset<16>(taskdata->td_affin_mask).to_string().c_str(),
               taskdata->td_task_place_tid));
}

///
/// @brief Update all thread's numa_head_start to their corresponding NUMA
/// base's thread_data deque head.
///
void Schedule::__kmp_set_head_all(kmp_task_team *task_team) {
  for (auto i = 0; i < task_team->tt.tt_nproc; ++i) {
    kmp_thread_data_t *thread_data = &task_team->tt.tt_threads_data[i];
    Schedule::__kmp_set_start_head(task_team, thread_data->td.td_thr, i);
  }
}

///
/// @brief Sets the numa_head_start of the thread. This is used to decide
/// whether a task will be marked for load balancing later.
///
void Schedule::__kmp_set_start_head(kmp_task_team_t *task_team,
                                    kmp_info_t *thread, kmp_int32 tid) {
  // const auto numa_base_tid = Schedule::__kmp_get_numa_base(tid);
  // auto place = thread->th.th_current_place;
  // KMP_DEBUG_ASSERT(place >=0);

  const auto gtid = __kmp_gtid_from_thread(thread);
  const auto numa_base_tid = ILAN::__kmp_ilan_topology().get_numa_master_tid(gtid);

  kmp_thread_data_t *threads_data =
      &task_team->tt.tt_threads_data[numa_base_tid];
  KMP_DEBUG_ASSERT(threads_data != NULL);
  thread->th.has_execed_on_self = 0;
  thread->th.numa_head_start = threads_data->td.td_deque_head;
  KD_TRACE(2, ("__kmp_set_start_head: Thread tid=%d , master_numa_tid=%d, "
               "numa_head_start=%u\n",
               tid, numa_base_tid, thread->th.numa_head_start));
}

///
/// @brief Decide if task should be marked for load balancing.
///
kmp_uint16
Schedule::__kmp_get_load_balance_mask(kmp_info_t *thread,
                                      kmp_thread_data_t *thread_data,
                                      const kmp_uint16 available_steal) {

  const auto coresPerNuma =
      ILAN::__kmp_ilan_topology().get_num_pus() / ILAN::__kmp_ilan_topology().get_num_numa();
  const auto numStrictTasks = coresPerNuma * NUM_LOAD_STRICT_TASK;
  // Distance of the current spot in queue in comparison to where queue started
  // this taskloop
  // @hongguang: td_deque is circular buffer, numa_head_start is the inital index (should be 0?)
  // & is equal to mod op.
  const auto distance =
      (thread_data->td.td_deque_tail - thread->th.numa_head_start +
       thread_data->td.td_deque_size) &
      TASK_DEQUE_MASK(thread_data->td);

  // If in strict range then disable load balancing for this task
  if (distance <= numStrictTasks) {
    return static_cast<kmp_uint16>(StealPolicy::NUMA);
  }
  return available_steal;
}



static void print_routine_map() {
  KD_TRACE(1, ("---- Routine Map START ----\n"));
  for (const auto &pair : g_routine_map) {
    const auto &routine_id = pair.first;
    const auto &routine = pair.second;
    KD_TRACE(1, ("\tRoutine %p:\n", routine_id));
  }
  KD_TRACE(1, ("----  Routine Map END ----\n"));
}


///
/// @brief Summarize and store the stats for the executed taskloop
///
void Schedule::__kmp_store_routine_stats(kmp_team *team, kmp_int64 routine_id) {
  if (team->t.t_nproc == 1) {
    KD_TRACE(
        1,
        ("__kmp_store_routine_stats: Only 1 thread, do not store stats. %p\n",
         routine_id));
    return;
  }

  // all nodes stats, even some nodes are not used
  routine_stats_nodes stats;

  // Get all node stats in this team (routine/taskloop)
  const kmp_real64 taskloop_start_time = Schedule::__kmp_get_routine_timer();
  Perf::__kmp_get_taskloop_stats(team, stats, taskloop_start_time);

  if(g_routine_map.find(routine_id) == g_routine_map.end()) {
    KD_TRACE(1, ("__kmp_store_routine_stats: "
      "cannot find routine %p in map, adding it now.\n",
                 routine_id));

    print_routine_map();
    // just return, @hongguang: have a btter way, 
    // incase tasloop tc == 0
    return;
  }

  // Verify that the routine exists in the map
  KMP_DEBUG_ASSERT(g_routine_map.find(routine_id) != g_routine_map.end());

  KD_TRACE(1, ("__kmp_store_routine_stats: New stat store for routine %p\n",
               routine_id));

  // Store the execution stats
  g_routine_map.at(routine_id).storeExecution(stats);
}



///
/// @brief Get the next config for the taskloop routine that is
/// about to be executed.
///
routine_config Schedule::__kmp_select_config(kmp_info *thread) {

  if (thread->th.th_team_nproc == 1) {
    // @todo hongguang: I don't know when th_team_nproc can be 1 here.
    KD_TRACE(1, ("__kmp_store_routine_stats: Select default "
                 "config={1,10,255,TASK_GEN}\n"));
    return routine_config{1,   // num_threads
                          10,  // num_tasks
                          static_cast<kmp_uint16>(StealPolicy::TASK_GENERATION), // node_mask
                          StealPolicy::TASK_GENERATION}; // steal_policy
  }

  routine_config ret_config;
  kmp_int64 routine_id = thread->th.routine_id;

  // Check if routine has executed before
  // If not, add new routine to map and return default config
  if (g_routine_map.find(routine_id) == g_routine_map.end()) {
    g_routine_map.emplace(routine_id,
                        Routine(routine_id, thread->th.th_team_nproc));
    // default routine:  m_current_config(getInitialConfig(nthreads))
    // inital config: 
    // num_threads = nthreads, num_tasks = 10*num_threads, 
    // node_mask = 1U << ILAN::__kmp_ilan_topology().get_num_numa()) - 1 // all numa nodes
    // steal_policy = StealPolicy::NUMA
    ret_config = g_routine_map.at(routine_id).getCurrentConfig();

  } else {
    ret_config = g_routine_map.at(routine_id).getNextConfig();
  }

  KD_TRACE(1,
           ("__kmp_select_config: routine %p was given new \n"
            "\tconfig={\n"
            "\t\t num threads:%d\n"
            "\t\t num tasks  :%d\n"
            "\t\t node mask  :0x%x\n"
            "\t\t plocy      :%d\n"
            "\t}.\n",
            routine_id, 
            ret_config.num_threads, 
            ret_config.num_tasks,
            ret_config.node_mask, 
            static_cast<int>(ret_config.steal_policy)));

  return ret_config;
}



///
/// @brief Start the timer for the current routine.
///
void Schedule::__kmp_start_routine_timer() {
  __kmp_read_system_time(&routine_timer);
}

///
/// @brief Gets the current value of the routine timer.
///
kmp_real64 Schedule::__kmp_get_routine_timer() { return routine_timer; }
