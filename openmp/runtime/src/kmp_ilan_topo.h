#pragma once

#include "kmp_os.h"
#include "kmp_affinity.h"

#include <unordered_map>
#include <unordered_set>

/*

  This file contains the ILANTopology class definition
  and related functions to manage the topology information

  for using OMP internal topology and affinity setting process.
  
  ILANTopology class holds the topology information used by ILAN scheduler,
  and provides mapping functions between gtid, os_id (PU id assigned by OS),
  numa_id, cpu_mask_idx, etc.

  mapping relationships:

    os_id (PU) <-> numa_id
    cpu_mask_idx/(thread's place) <-> os_id (PU)
    gtid (with single cpu affinity mask) -> os_id (PU)

  ILAN should work with assumption beload:
  1.each thread's affinity mask contains only one PU (os_id)
  2.a PU can hold more than one thread (in oversubscription case)
  3.parallel regions don't need to use all available threads/cores
  4.parallel regions can use threads more than available cores (oversubscription)
  5.thread should not rebinding to different PU during after it created. 

*/


class ILANTopology {
public:
  ILANTopology() = default;

  ILANTopology(kmp_topology_t* global_toppology);

  kmp_uint32 get_num_socket() const { return num_sockets_; }
  kmp_uint32 get_num_numa() const { return num_numa_; }
  kmp_uint32 get_num_pus() const { return num_cores_; }
  
  kmp_uint32 get_num_total_sockets() const { return total_num_sockets_; }
  kmp_uint32 get_num_total_numa() const { return total_num_numa_; }
  kmp_uint32 get_num_total_pus() const { return total_num_pus_; }

  
  // the numa size is not the real numa size in the system
  // but the number of numa nodes used by OpenMP runtime
  // e.g., if the system has 8 cores per numa node,
  // but OpenMP runtime only uses 4 cores per numa node
  // then the numa size here is 4
  kmp_uint8 get_numa_size() const { return numa_size_; }

  kmp_uint16 get_numa_mask() const { return ilan_numa_set_; }


  void showTopo() const;

  /*****************  id mapping functions start ****************/ 

  // because ilan_cpu_set_ |= (1 << os_id);, so 
  // os_id is 1-to-1 mapped to cpu_mask_idx/place
  // this is called when a thread is created and its affinity mask is assigned
  // in : 
  // and for root thread, in:__kmp_parallel_initialize
  void register_gtid_os_id_map(kmp_int32 gtid, kmp_uint32 os_id) {
    gtid_to_os_id_[gtid] = os_id;

    if (os_id_to_gtids_.find(os_id) == os_id_to_gtids_.end()) {
      os_id_to_gtids_[os_id] = std::unordered_set<kmp_uint32>();
    }
    os_id_to_gtids_[os_id].insert(gtid);
  }

  // given a gtid, return its numa id
  kmp_uint32 get_numa_id(kmp_int32 gtid);

  // given a gtid, return its ilan_master tid
  // why tid here? 
  // because we want the master of numa in where threads in the same team
  kmp_uint32 get_numa_master_tid(kmp_int32 gtid);


  // given two gtids, check if they are in the same numa node
  bool is_same_numa_node(kmp_int32 gtid1, kmp_int32 gtid2);



  /*****************  id mapping functions end ****************/ 


  /**  mask mapping functions */

  // get the numa id with a mask index (place)
  // the index alwasy starts from 0, up to numa_size -1
  // we need it, because the stats array in kmp_ilan_routine.cpp
  // is indexed by 0 ... numa_size -1
  // but the numa id maybe not continuous
  kmp_uint32 get_numa_id_from_index(kmp_uint64 index /*place*/){
    KMP_DEBUG_ASSERT(index < numa_size_);
    kmp_uint32 count = 0;
    for (kmp_uint32 numa_id = 0; numa_id < total_num_numa_; numa_id++) {
      // check if this numa_id is in the ilan_numa_set_
      if (ilan_numa_set_ & (1ULL << numa_id)) {
        if (count == index) {
          return numa_id;
        }
        count++;
      }
    }
    // should not reach here
    KMP_DEBUG_ASSERT(false);
    return UINT32_MAX;
  }


private:
  // we save a pointer to the global kmp topology structure
  // for easy access to other topology info if needed
  kmp_topology_t* kmp_topology_;

  // these values are passed from openmp global topology structure
  kmp_uint32 num_sockets_;    // number of sockets used by OpenMP runtime
  kmp_uint32 num_numa_;       // number of NUMA nodes used by OpenMP runtime
  kmp_uint32 num_cores_;      // number of cores used by OpenMP runtime
  kmp_uint32 num_threads_;    // number of threads used by OpenMP runtime  // PUs

  // for ILAN scheduler, finding the fastest numa node mask index
  kmp_uint8 numa_size_;        // number of cores per NUMA node used by OpenMP runtime

  // now just consider bitmask up to 64 CPUs/NUMA nodes
  kmp_uint64 ilan_cpu_set_;  // bitmask of CPUs used by OpenMP runtime
  kmp_uint64 ilan_numa_set_; // bitmask of NUMA nodes used by OpenMP runtime

  /****** global system info start *****/
  // the total number of sockets/numa/cores in the system
  // but maybe not all used by OpenMP runtime, due to 
  // affinity setting from outside(OpenMP runtime)
  kmp_uint32 total_num_sockets_;   // total number of sockets in the system
  kmp_uint32 total_num_numa_;      // total number of numa nodes in the system
  kmp_uint32 total_num_cores_;     // total number of cores in the system
  kmp_uint32 total_num_pus_;       // total number of processing units in the system
  


  /*
    every thing mapped like this:
    ILAN level mapping:
      os_id (PU) <-> numa_id
      cpu_mask_idx <-> os_id (PU)
      gtid (with single cpu affinity mask) -> os_id (PU)

    OpenMP level mapping:
      tid <-> gtid
  */
  // mapping from os_id (PU/thread) to numa_id
  // key: os_id, value: numa_id , both are system wide id
  // os_id is the PU id assigned by OS
  std::unordered_map<kmp_uint32, kmp_uint32> os_id_to_numa_id_; 

  // mapping from numa_id to os_ids (PU/thread)
  // key: numa_id, value: list of os_ids , both are system wide id(physical id)
  // but it just holds the os_ids used by OpenMP runtime in each numa node
  std::unordered_map<kmp_uint32, std::vector<kmp_uint32>> numa_id_to_os_ids_;

  // mapping from gtid to os_id
  // this is set when a thread is created and the affinity mask is assigned
  // in: __kmp_affinity_set_init_mask
  // and for root thread, in: ?
  std::unordered_map<kmp_uint32, kmp_uint32> gtid_to_os_id_;
  // mapping from os_id to gtids, because multiple threads can be
  // bound to the same os_id (PU) in oversubscription case. but mostly
  // it is 1-to-1 mapping
  std::unordered_map<kmp_uint32, std::unordered_set<kmp_uint32>> os_id_to_gtids_;

  // read full system topology using hwloc
  // used to updadet os_id_to_numa_id_ map
  void init_read_full_topology();

  /****** global system info end *****/
};

namespace ILAN {

// Topology part
// ILANTopology __kmp_read_topology();
// extern const ILANTopology __kmp_ilan_topology();
ILANTopology& __kmp_ilan_topology();

// will check and register the global topology used by ILAN scheduler to ILAN topology structure
// and if the affinity is not set, it will set it forcely to close,bind. 
// called in: kmp_affinity.cpp : __kmp_affinity_initialize
void __kmp_ilan_sync_affinity_topology(kmp_affinity_t *affinity, kmp_topology_t* global_toppology);

} // namespace ILAN