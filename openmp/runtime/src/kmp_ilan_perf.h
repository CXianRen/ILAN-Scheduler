#pragma once

#include "kmp_os.h"

#include <vector>

////////////////////////////////
///   Forward declarations   ///
////////////////////////////////
union kmp_info;
union kmp_team;

enum class PerfEvents : int { TOT_CYCLES = 0, TOT_INSTRUCTIONS = 1 };

constexpr int32_t NUM_PERF_EVENTS = 2;

struct routine_stats {
  kmp_real64 execution_time;
  kmp_real64 IPC;
};

using routine_stats_nodes = std::vector<routine_stats>;


namespace Perf {

/*
    Initialize performance counters for a thread
    this should be called once the thread is binded to a CPU
    now is called by: __kmp_launch_thread
    after __kmp_fork_barrier
    
    in a thread callback context:
    1) call __kmp_ilan_init_counters to init the counters
    2) while - loop of work (here the counters are running)
    3) call __kmp_ilan_deinit_counters to close the counters
*/
void __kmp_ilan_init_counters(kmp_info *thread, int32_t gtid);

/*
    close performance counters for a thread
*/
void __kmp_ilan_deinit_counters(kmp_info *thread);

/*
    Start performance counters for a thread
    now is called by: __kmp_task_start

    when it will set the timestamp as current time
    and enable the counters

    call like:
        start task: call __kmp_ilan_start_counters
            execute task
        finish task: call __kmp_ilan_stop_counters
    
*/
void __kmp_ilan_start_counters(kmp_info *thread);

/*
    Stop performance counters for a thread
    now is called by: __kmp_task_finish

    when it called, it will read the counters and
    accumulate the values to thread->th.perf_accum
    then reset the counters to zero
*/
void __kmp_ilan_stop_counters(kmp_info *thread, int32_t gtid, kmp_int32 task_id);


// void __kmp_summarize_taskloop_numa(kmp_team *team,
//                                    kmp_real64 taskloop_start_time);

/*
    Called by: __kmp_store_routine_stats
    summarize the  thread->th.perf_accum of each thread in a team
    and in a same NUMA node, to get the IPC and execution time
*/
void __kmp_get_taskloop_stats(kmp_team *team, routine_stats_nodes &stats,
                              const kmp_real64 taskloop_start_time);

} // namespace Perf