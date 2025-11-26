#pragma once

#include "kmp.h"
#include "kmp_os.h"
#include "kmp_ilan_routine.h"
#include "kmp_ilan_topo.h"

#include "kmp_affinity.h"

////////////////////////////////
///   Forward declarations   ///
////////////////////////////////
union kmp_info;
union kmp_team;
union kmp_task_team;
union kmp_thread_data;
struct kmp_taskdata;
struct kmp_affinity_t;

namespace Schedule {
struct PolicyInfo {
  explicit PolicyInfo(kmp_uint16 node_mask, kmp_uint16 available_steal_mask)
      : node_mask(node_mask), available_steal_mask(available_steal_mask) {};
  kmp_uint16 node_mask;  // which NUMA nodes are allowed for running the task
  kmp_uint16 available_steal_mask;  // which NUMA nodes are allowed for stealing
};

// get policy info for the given routine, and use it to make scheduling decisions
// e.g __kmp_set_task_affinity
// which numa nodes are allowed for running the task
// which numa nodes are allowed for a thread to steal tasks from


/**
 *  called by kmp_tasking.cpp: __kmp_taskloop_linear
 *  (Create a taskloop with linear scheduling)
 *  before calling __kmp_set_task_affinity
 *  This function gets the policy info for the given routine 
 *  (number of node will be used, etc.)  
 *  And this will pass the info to __kmp_set_task_affinity
 *  to generate the task's affinity
 **/

PolicyInfo __kmp_get_policy_info(kmp_info *thread, kmp_int64 routine_id);

/**
 *  called by kmp_tasking.cpp: __kmp_taskloop_linear 
 *  (Create a taskloop with linear scheduling)
 *  This function sets the task's affinity based on the 
 *  1. policy info : which numa nodes, balance mask, etc.
 *  2. the task's iteration range (lb, ub), assuming 
 *    tasks nearby have data locality
 */
void __kmp_set_task_affinity(kmp_info *thread, kmp_taskdata *taskdata,
                             kmp_int64 routine_id, const PolicyInfo &policyInfo,
                             kmp_uint64 lb, kmp_uint64 ub, kmp_uint64 glob_ub);


// Scheduling decisions
/**
 *  called by kmp_tasking.cpp: __kmp_push_task (Add a task to the thread's deque)
 *  This function selects which thread's deque to push the task into
 *  based on the task's affinity 
 * */ 
kmp_thread_data *__kmp_select_thread_data_queue(kmp_task_team *task_team,
                                                kmp_taskdata *taskdata);
                 

/**
 * to record the head index of the master thread's queue in a numa node
 * for task balancing.
 *  called by kmp_tasking.cpp: __kmp_taskloop() (Create a taskloop)
 *  also called by __kmp_enable_tasking() in kmp_tasking.cpp
 */

void __kmp_set_head_all(kmp_task_team *task_team);
void __kmp_set_start_head(kmp_task_team *task_team, kmp_info *thread,
                          kmp_int32 tid);
// used only by kmp_shcedule.cpp
kmp_uint16 __kmp_get_load_balance_mask(kmp_info *thread,
                                       kmp_thread_data *thread_data,
                                       kmp_uint16 available_steal);

// used only for debugging
// void __kmp_show_affinity(kmp_info *thread);
// void __kmp_set_per_thread_affinity(kmp_info *thread, int32_t gtid);

// Routine part
void __kmp_store_routine_stats(kmp_team *team, kmp_int64 routine_id);
routine_config __kmp_select_config(kmp_info *thread);
void __kmp_start_routine_timer();
kmp_real64 __kmp_get_routine_timer();



} // namespace Schedule
